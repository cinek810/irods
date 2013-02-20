/* -*- mode: c++; fill-column: 132; c-basic-offset: 4; indent-tabs-mode: nil -*- */

/*** Copyright (c), The Regents of the University of California            ***
 *** For more information please refer to files in the COPYRIGHT directory ***/

/* physPath.c - routines for physical path operations
 */

#include <unistd.h> // JMC - backport 4598
#include <fcntl.h> // JMC - backport 4598
#include "rodsDef.h"
#include "physPath.h"
#include "dataObjOpr.h"
#include "rodsDef.h"
#include "rsGlobalExtern.h"
#include "fileChksum.h"
#include "modDataObjMeta.h"
#include "objMetaOpr.h"
#include "collection.h"
#include "resource.h"
#include "dataObjClose.h"
#include "rcGlobalExtern.h"
#include "reGlobalsExtern.h"
#include "reDefines.h"
#include "reSysDataObjOpr.h"
#include "genQuery.h"
#include "rodsClient.h"

#include <iostream>

// =-=-=-=-=-=-=-
// eirods include
#include "eirods_resource_backport.h"
#include "eirods_hierarchy_parser.h"
#include "eirods_stacktrace.h"

int
getFileMode (dataObjInp_t *dataObjInp)
{
    int createMode;
    int defFileMode;

    defFileMode = getDefFileMode ();
    if (dataObjInp != NULL && 
        (dataObjInp->createMode & 0110) != 0) {
        if ((defFileMode & 0070) != 0) {
            createMode = defFileMode | 0110;
        } else {
            createMode = defFileMode | 0100;
        }
    } else {
        createMode = defFileMode;
    }

    return (createMode);
}

int
getFileFlags (int l1descInx)
{
    int flags;

    dataObjInp_t *dataObjInp = L1desc[l1descInx].dataObjInp;

    if (dataObjInp != NULL) { 
        flags = dataObjInp->openFlags;
    } else {
        flags = O_RDONLY;
    }

    return (flags);
}

int
getFilePathName (rsComm_t *rsComm, dataObjInfo_t *dataObjInfo,
                 dataObjInp_t *dataObjInp)
{
    char *filePath;
    vaultPathPolicy_t vaultPathPolicy;
    int status;

    if (dataObjInp != NULL && 
        (filePath = getValByKey (&dataObjInp->condInput, FILE_PATH_KW)) != NULL 
        && strlen (filePath) > 0) {
        rstrcpy (dataObjInfo->filePath, filePath, MAX_NAME_LEN);
        return (0);
    }

    /* Make up a physical path */ 
    if (dataObjInp != NULL && dataObjInfo->rescInfo == NULL) {
        rodsLog (LOG_ERROR,
                 "getFilePathName: rescInfo for %s not resolved", 
                 dataObjInp->objPath);
        return (SYS_INVALID_RESC_INPUT);
    }

    // JMC - legacy resource if (RescTypeDef[dataObjInfo->rescInfo->rescTypeInx].createPathFlag == NO_CREATE_PATH) {
    int chk_path = 0;
    eirods::error err = eirods::get_resource_property< int >( dataObjInfo->rescInfo->rescName, "check_path_perm", chk_path );
    if( !err.ok() ) {
        eirods::log( PASS( false, -1, "getFilePathName - failed.", err ) );
    }

    if( NO_CREATE_PATH == chk_path ) {
        *dataObjInfo->filePath = '\0';
        return 0;
    }

    std::string vault_path;
    status = getLeafRescPathName(dataObjInfo->rescHier, vault_path);
    if(status != 0) {
        return status;
    }
    
    status = getVaultPathPolicy (rsComm, dataObjInfo, &vaultPathPolicy);
    if (status < 0) {
        return (status);
    }

    if (vaultPathPolicy.scheme == GRAFT_PATH_S) {
        status = setPathForGraftPathScheme (dataObjInp->objPath, 
                                            vault_path.c_str(), vaultPathPolicy.addUserName,
                                            rsComm->clientUser.userName, vaultPathPolicy.trimDirCnt, 
                                            dataObjInfo->filePath);
    } else {
        status = setPathForRandomScheme (dataObjInp->objPath,
                                         vault_path.c_str(), rsComm->clientUser.userName,
                                         dataObjInfo->filePath);
    }
    if (status < 0) {
        return (status);
    }

    return (status);
}

int
getVaultPathPolicy (rsComm_t *rsComm, dataObjInfo_t *dataObjInfo,
                    vaultPathPolicy_t *outVaultPathPolicy)
{
    ruleExecInfo_t rei;
    msParam_t *msParam;
    int status;

    if (outVaultPathPolicy == NULL || dataObjInfo == NULL || rsComm == NULL) {
        rodsLog (LOG_ERROR,
                 "getVaultPathPolicy: NULL input");
        return (SYS_INTERNAL_NULL_INPUT_ERR);
    } 
    initReiWithDataObjInp (&rei, rsComm, NULL);
   
    rei.doi = dataObjInfo;
    status = applyRule ("acSetVaultPathPolicy", NULL, &rei, NO_SAVE_REI);
    if (status < 0) {
        rodsLog (LOG_ERROR,
                 "getVaultPathPolicy: rule acSetVaultPathPolicy error, status = %d",
                 status);
        return (status);
    }

    if ((msParam = getMsParamByLabel (&rei.inOutMsParamArray,
                                      VAULT_PATH_POLICY)) == NULL) {
        /* use the default */
        outVaultPathPolicy->scheme = DEF_VAULT_PATH_SCHEME;
        outVaultPathPolicy->addUserName = DEF_ADD_USER_FLAG;
        outVaultPathPolicy->trimDirCnt = DEF_TRIM_DIR_CNT;
    } else {
        *outVaultPathPolicy = *((vaultPathPolicy_t *) msParam->inOutStruct);
        clearMsParamArray (&rei.inOutMsParamArray, 1);
    }
    /* make sure trimDirCnt is <= 1 */
    if (outVaultPathPolicy->trimDirCnt > DEF_TRIM_DIR_CNT)
        outVaultPathPolicy->trimDirCnt = DEF_TRIM_DIR_CNT;

    return (0);
}

int 
setPathForRandomScheme (char *objPath, const char *vaultPath, char *userName,
                        char *outPath)
{
    int myRandom;
    int dir1, dir2;
    char logicalCollName[MAX_NAME_LEN];
    char logicalFileName[MAX_NAME_LEN];
    int status;

    myRandom = random (); 
    dir1 = myRandom & 0xf;
    dir2 = (myRandom >> 4) & 0xf;

    status = splitPathByKey(objPath,
                            logicalCollName, logicalFileName, '/');

    if (status < 0) {
        rodsLog (LOG_ERROR,
                 "setPathForRandomScheme: splitPathByKey error for %s, status = %d",
                 outPath, status);
        return (status);
    }

    snprintf (outPath, MAX_NAME_LEN,
              "%s/%s/%d/%d/%s.%d", vaultPath, userName, dir1, dir2, 
              logicalFileName, (uint) time (NULL));
    return (0);
}

int 
setPathForGraftPathScheme (char *objPath, const char *vaultPath, int addUserName,
                           char *userName, int trimDirCnt, char *outPath)
{
    int i;
    char *objPathPtr, *tmpPtr;
    int len;

    objPathPtr = objPath + 1;

    for (i = 0; i < trimDirCnt; i++) {
        tmpPtr = strchr (objPathPtr, '/');
        if (tmpPtr == NULL) {
            rodsLog (LOG_ERROR,
                     "setPathForGraftPathScheme: objPath %s too short", objPath);
            break;      /* just use the shorten one */
        } else {
            /* skip over '/' */
            objPathPtr = tmpPtr + 1;
            /* don't skip over the trash path */
            if (i == 0 && strncmp (objPathPtr, "trash/", 6) == 0) break; 
        }
    }

    if (addUserName > 0 && userName != NULL) {
        len = snprintf (outPath, MAX_NAME_LEN,
                        "%s/%s/%s", vaultPath, userName, objPathPtr);
    } else {
        len = snprintf (outPath, MAX_NAME_LEN,
                        "%s/%s", vaultPath, objPathPtr);
    }

    if (len >= MAX_NAME_LEN) {
        rodsLog (LOG_ERROR,
                 "setPathForGraftPathScheme: filePath %s too long", objPath);
        return (USER_STRLEN_TOOLONG);
    } else {
        return (0);
    }
}

/* resolveDupFilePath - try to resolve deplicate file path in the same
 * resource.
 */

int
resolveDupFilePath (rsComm_t *rsComm, dataObjInfo_t *dataObjInfo,
                    dataObjInp_t *dataObjInp)
{
    char tmpStr[NAME_LEN];
    char *filePath;

    if (getSizeInVault (rsComm, dataObjInfo) == SYS_PATH_IS_NOT_A_FILE) {
        /* a dir */
        return (SYS_PATH_IS_NOT_A_FILE);
    }
    if (chkAndHandleOrphanFile (rsComm, dataObjInfo->objPath, dataObjInfo->rescHier, dataObjInfo->filePath, 
                                dataObjInfo->rescInfo, dataObjInfo->replStatus) >= 0) {
        /* this is an orphan file or has been renamed */
        return 0;
    }

    if (dataObjInp != NULL) {
        filePath = getValByKey (&dataObjInp->condInput, FILE_PATH_KW);
        if (filePath != NULL && strlen (filePath) > 0) {
            return -1;
        }
    }

    if (strlen (dataObjInfo->filePath) >= MAX_NAME_LEN - 3) {
        return -1;
    }

    snprintf (tmpStr, NAME_LEN, ".%d", dataObjInfo->replNum);
    strcat (dataObjInfo->filePath, tmpStr);

    return (0);
}

int
getchkPathPerm (rsComm_t *rsComm, dataObjInp_t *dataObjInp, 
                dataObjInfo_t *dataObjInfo)
{
    int chkPathPerm;
    char *filePath;
    rescInfo_t *rescInfo;
    ruleExecInfo_t rei;

    if (rsComm->clientUser.authInfo.authFlag == LOCAL_PRIV_USER_AUTH) {
        return (NO_CHK_PATH_PERM);
    }

    if (dataObjInp == NULL || dataObjInfo == NULL) {
        return (NO_CHK_PATH_PERM);
    }

    rescInfo = dataObjInfo->rescInfo;

    if ((filePath = getValByKey (&dataObjInp->condInput, FILE_PATH_KW)) != NULL 
        && strlen (filePath) > 0) {
        /* the user input a path */
        if (rescInfo == NULL) {
            chkPathPerm = NO_CHK_PATH_PERM;
        } else {
            initReiWithDataObjInp (&rei, rsComm, dataObjInp);
            rei.doi = dataObjInfo;
            // =-=-=-=-=-=-=-
            // JMC - backport 4774
            rei.status = DISALLOW_PATH_REG;             /* default */
            applyRule ("acSetChkFilePathPerm", NULL, &rei, NO_SAVE_REI);

            int chk_path = 0;
            eirods::error err = eirods::get_resource_property< int >( rescInfo->rescName, "check_path_perm", chk_path );
            if( !err.ok() ) {
                eirods::log( PASS( false, -1, "getchkPathPerm - failed.", err ) );
            }

            if( err.ok() && ( rei.status == NO_CHK_PATH_PERM || NO_CHK_PATH_PERM == chk_path ) ) {
// JMC - legacy resource RescTypeDef[rescInfo->rescTypeInx].chkPathPerm == NO_CHK_PATH_PERM ) ) {
                chkPathPerm = NO_CHK_PATH_PERM;
            } else {
                chkPathPerm = rei.status;
                // =-=-=-=-=-=-=-
            }
        }
    } else {
        chkPathPerm = NO_CHK_PATH_PERM;
    }
    return (chkPathPerm);
}

int 
getCopiesFromCond (keyValPair_t *condInput)
{
    char *myValue;

    myValue = getValByKey (condInput, COPIES_KW);

    if (myValue == NULL) {
        return (1);
    } else if (strcmp (myValue, "all") == 0) {
        return (ALL_COPIES);
    } else {
        return (atoi (myValue));
    }
}

int
getWriteFlag (int openFlag)
{
    if (openFlag & O_WRONLY || openFlag & O_RDWR) {
        return (1);
    } else {
        return (0);
    }
}

rodsLong_t 
getSizeInVault (rsComm_t *rsComm, dataObjInfo_t *dataObjInfo)
{
    rodsStat_t *myStat = NULL;
    int status;
    rodsLong_t mysize;

    status = l3Stat (rsComm, dataObjInfo, &myStat);

    if (status < 0 || NULL == myStat) { // JMC cppcheck - nullptr
        rodsLog (LOG_DEBUG,
                 "getSizeInVault: l3Stat error for %s. status = %d",
                 dataObjInfo->filePath, status);
        return (status);
    } else {
        if (myStat->st_mode & S_IFDIR) {
            return ((rodsLong_t) SYS_PATH_IS_NOT_A_FILE);
        }
        mysize = myStat->st_size;
        free (myStat);
        return (mysize);
    }
}

int 
_dataObjChksum ( rsComm_t *rsComm, dataObjInfo_t *inpDataObjInfo, char **chksumStr) // JMC - backport 4527
{
    fileChksumInp_t fileChksumInp;
    int rescClass = 0;
    int status = 0;
    // =-=-=-=-=-=-=-
    // JMC - backport 4527
    dataObjInfo_t *dataObjInfo; 
    rescInfo_t *rescInfo = inpDataObjInfo->rescInfo;
    int destL1descInx = -1;

#if 0 // JMC - removing compound resources 
    if (rescClass == COMPOUND_CL) {
#if 0
        return SYS_CANT_CHKSUM_COMP_RESC_DATA;
#else
        dataObjInp_t dataObjInp;
        status = getCacheRescInGrp (rsComm, inpDataObjInfo->rescGroupName,inpDataObjInfo->rescInfo, &cacheResc);
        if (status < 0) {
            rodsLog (LOG_ERROR,
                     "_dataObjChksum: getCacheRescInGrp %s failed for %s stat=%d",
                     inpDataObjInfo->rescGroupName, inpDataObjInfo->objPath, status);
            return status;
        }
        /* create a fake object */
        memset (&dataObjInp, 0, sizeof (dataObjInp_t));
        snprintf (dataObjInp.objPath, MAX_NAME_LEN, "%s.%-d", 
                  inpDataObjInfo->objPath, (int) random());
        addKeyVal (&dataObjInp.condInput, DEST_RESC_NAME_KW, 
                   cacheResc->rescName);
        addKeyVal (&dataObjInp.condInput, NO_OPEN_FLAG_KW, "");
        destL1descInx = _rsDataObjCreate (rsComm, &dataObjInp);
        clearKeyVal (&dataObjInp.condInput);
        if (destL1descInx < 0) {
            rodsLogError (LOG_ERROR, destL1descInx,
                          "_dataObjChksum: _rsDataObjCreate failed for %s",
                          inpDataObjInfo->objPath);
            return destL1descInx;
        }
        dataObjInfo = L1desc[destL1descInx].dataObjInfo;
        rescInfo = cacheResc;
        status = _l3FileStage (rsComm, inpDataObjInfo, dataObjInfo,
                               getFileMode (NULL));
        if (status < 0) {
            rodsLogError (LOG_ERROR, status,
                          "_dataObjChksum: _l3FileStage failed for %s",
                          dataObjInfo->objPath);
            return destL1descInx;
        }
#endif
    } else if (rescClass == BUNDLE_CL) {
        return SYS_CANT_CHKSUM_BUNDLED_DATA;
    } else 
#endif // JMC
    
        if (rescClass == BUNDLE_CL) {
            return SYS_CANT_CHKSUM_BUNDLED_DATA;
        } else {
            dataObjInfo = inpDataObjInfo;
        }

    // =-=-=-=-=-=-=-
    int category = 0;
    eirods::error err = eirods::get_resource_property< int >( rescInfo->rescName, "category", category );
    if( !err.ok() ) {
        eirods::log( PASS( false, -1, "_dataObjChksum - failed.", err ) );
    }

    // JMC - legacy resource - switch ( RescTypeDef[rescTypeInx].rescCat) {
    switch( category ) {
    case FILE_CAT:
        memset (&fileChksumInp, 0, sizeof (fileChksumInp));
        fileChksumInp.fileType = static_cast< fileDriverType_t >( -1 );// JMC - legacy resource - (fileDriverType_t)RescTypeDef[rescTypeInx].driverType;
        rstrcpy (fileChksumInp.addr.hostAddr, rescInfo->rescLoc,NAME_LEN);
        rstrcpy (fileChksumInp.fileName, dataObjInfo->filePath, MAX_NAME_LEN);
        rstrcpy (fileChksumInp.rescHier, dataObjInfo->rescHier, MAX_NAME_LEN);
        rstrcpy (fileChksumInp.objPath, dataObjInfo->objPath, MAX_NAME_LEN);
        status = rsFileChksum (rsComm, &fileChksumInp, chksumStr);
        break;
    default:
        rodsLog (LOG_NOTICE,
                 "_dataObjChksum: rescCat type %d is not recognized", category );
        status = SYS_INVALID_RESC_TYPE;
        break;
    }
    // =-=-=-=-=-=-=-
    // JMC - backport 4527
    if (destL1descInx >= 0) {
        l3Unlink (rsComm, L1desc[destL1descInx].dataObjInfo);
        freeL1desc (destL1descInx);
    }

    return (status);
}

int
dataObjChksumAndReg (rsComm_t *rsComm, dataObjInfo_t *dataObjInfo, 
                     char **chksumStr) 
{
    keyValPair_t regParam;
    modDataObjMeta_t modDataObjMetaInp;
    int status;

    status = _dataObjChksum (rsComm, dataObjInfo, chksumStr);
    if (status < 0) {
        rodsLog (LOG_NOTICE,
                 "dataObjChksumAndReg: _dataObjChksum error for %s, status = %d",
                 dataObjInfo->objPath, status);
        return (status);
    }

    /* register it */
    memset (&regParam, 0, sizeof (regParam));
    addKeyVal (&regParam, CHKSUM_KW, *chksumStr);

    modDataObjMetaInp.dataObjInfo = dataObjInfo;
    modDataObjMetaInp.regParam = &regParam;

    status = rsModDataObjMeta (rsComm, &modDataObjMetaInp);

    clearKeyVal (&regParam);

    if (status < 0) {
        rodsLog (LOG_NOTICE,
                 "dataObjChksumAndReg: rsModDataObjMeta error for %s, status = %d",
                 dataObjInfo->objPath, status);
        /* don't return error because it is not fatal */
    }

    return (0);
}

/* chkAndHandleOrphanFile - Check whether the file is an orphan file.
 * If it is, rename it.  
 * If it belongs to an old copy, move the old path and register it.
 *
 * return 0 - the filePath is NOT an orphan file.
 *        1 - the filePath is an orphan file and has been renamed.
 *        < 0 - error
 */

int
chkAndHandleOrphanFile (rsComm_t *rsComm, char* objPath, char* rescHier, char *filePath, rescInfo_t *rescInfo,
                        int replStatus)
{

    fileRenameInp_t fileRenameInp;
    int status;
    dataObjInfo_t myDataObjInfo;

    int category = 0;
    eirods::error err = eirods::get_resource_property< int >( rescInfo->rescName, "category", category );
    if( !err.ok() ) {
        eirods::log( PASS( false, -1, "chkAndHandleOrphanFile - failed.", err ) );
    }

    // JMC - legacy resource  - if (RescTypeDef[rescTypeInx].rescCat != FILE_CAT) {
    if( FILE_CAT != category ) {
        /* can't do anything with non file type */
        return (-1);
    }

    if (strlen (filePath) + 17 >= MAX_NAME_LEN) {
        /* the new path name will be too long to add "/orphan + random" */
        return (-1);
    }
 
    /* check if the input filePath is assocated with a dataObj */

    memset (&myDataObjInfo, 0, sizeof (myDataObjInfo));
    memset (&fileRenameInp, 0, sizeof (fileRenameInp));
    if ((status = chkOrphanFile (rsComm, filePath, rescInfo->rescName, &myDataObjInfo)) == 0) {
        rstrcpy (fileRenameInp.oldFileName, filePath, MAX_NAME_LEN);
        rstrcpy (fileRenameInp.rescHier, rescHier, MAX_NAME_LEN);
        rstrcpy(fileRenameInp.objPath, objPath, MAX_NAME_LEN);
        
        /* not an orphan file */
        if (replStatus > OLD_COPY || isTrashPath (myDataObjInfo.objPath)) {
            modDataObjMeta_t modDataObjMetaInp;
            keyValPair_t regParam;

            /* a new copy or the current path is in trash. 
             * rename and reg the path of the old one */
            status = renameFilePathToNewDir (rsComm, REPL_DIR, &fileRenameInp, 
                                             rescInfo, 1);
            if (status < 0) {
                return (status);
            }
            /* register the change */
            memset (&regParam, 0, sizeof (regParam));
            addKeyVal (&regParam, FILE_PATH_KW, fileRenameInp.newFileName);
            modDataObjMetaInp.dataObjInfo = &myDataObjInfo;
            modDataObjMetaInp.regParam = &regParam;
            status = rsModDataObjMeta (rsComm, &modDataObjMetaInp);
            clearKeyVal (&regParam);
            if (status < 0) {
                rodsLog (LOG_ERROR,
                         "chkAndHandleOrphan: rsModDataObjMeta of %s error. stat = %d",
                         fileRenameInp.newFileName, status);
                /* need to rollback the change in path */
                rstrcpy (fileRenameInp.oldFileName, fileRenameInp.newFileName, 
                         MAX_NAME_LEN);
                rstrcpy (fileRenameInp.newFileName, filePath, MAX_NAME_LEN);
                rstrcpy(fileRenameInp.rescHier, myDataObjInfo.rescHier, MAX_NAME_LEN);
                status = rsFileRename (rsComm, &fileRenameInp);

                if (status < 0) {
                    rodsLog (LOG_ERROR,
                             "chkAndHandleOrphan: rsFileRename %s failed, status = %d",
                             fileRenameInp.oldFileName, status);
                    return (status);
                }
                /* this thing still failed */
                return (-1);
            } else {
                return (0);
            }
        } else {
            /* this is an old copy. change the path but don't
             * actually rename it */
            rstrcpy (fileRenameInp.oldFileName, filePath, MAX_NAME_LEN);
            status = renameFilePathToNewDir (rsComm, REPL_DIR, &fileRenameInp,
                                             rescInfo, 0);
            if (status >= 0) {
                rstrcpy (filePath, fileRenameInp.newFileName, MAX_NAME_LEN);
                return (0);
            } else {
                return (status);
            }
        }

    } else if (status > 0) {
        /* this is an orphan file. need to rename it */
        rstrcpy (fileRenameInp.oldFileName, filePath,           MAX_NAME_LEN);
        rstrcpy (fileRenameInp.rescHier,    rescHier,           MAX_NAME_LEN);
        rstrcpy (fileRenameInp.objPath,     objPath,            MAX_NAME_LEN);
        status = renameFilePathToNewDir (rsComm, ORPHAN_DIR, &fileRenameInp, 
                                         rescInfo, 1);
        if (status >= 0) {
            return (1);
        } else {
            return (status);
        }
    } else {
        /* error */
        return (status);
    }
}

int
renameFilePathToNewDir (rsComm_t *rsComm, char *newDir,
                        fileRenameInp_t *fileRenameInp, rescInfo_t *rescInfo, int renameFlag)
{
    int len, status;
    char *oldPtr, *newPtr;
    char *filePath = fileRenameInp->oldFileName;
    // JMC - legacy resource - fileRenameInp->fileType = (fileDriverType_t)RescTypeDef[rescTypeInx].driverType;

    rstrcpy (fileRenameInp->addr.hostAddr, rescInfo->rescLoc, NAME_LEN);

    len = strlen (rescInfo->rescVaultPath);

    if (len <= 0) {
        return (-1);
    }
    
    if (strncmp (filePath, rescInfo->rescVaultPath, len) != 0) {
        /* not in rescVaultPath */
        return -1;
    }

    rstrcpy (fileRenameInp->newFileName, rescInfo->rescVaultPath, MAX_NAME_LEN);
    oldPtr = filePath + len;
    newPtr = fileRenameInp->newFileName + len;

    snprintf (newPtr, MAX_NAME_LEN - len, "/%s%s.%-d", newDir, oldPtr, 
              (uint) random());
    
    if (renameFlag > 0) {
        status = rsFileRename (rsComm, fileRenameInp);
        if (status < 0) {
            rodsLog (LOG_NOTICE,
                     "renameFilePathToNewDir:rsFileRename from %s to %s failed,stat=%d",
                     filePath, fileRenameInp->newFileName, status);
            return -1;
        } else {
            return (0); 
        }
    } else {
        return (0);
    }
}

/* syncDataObjPhyPath - sync the path of the phy path with the path of
 * the data ovject. This is unsed by rename to sync the path of the
 * phy path with the new path.
 */

int
syncDataObjPhyPath (rsComm_t *rsComm, dataObjInp_t *dataObjInp,
                    dataObjInfo_t *dataObjInfoHead, char *acLCollection)
{
    dataObjInfo_t *tmpDataObjInfo;
    int status;
    int savedStatus = 0;

    tmpDataObjInfo = dataObjInfoHead;
    while (tmpDataObjInfo != NULL) {
        status = syncDataObjPhyPathS( rsComm, dataObjInp, tmpDataObjInfo, acLCollection );
         
        if (status < 0) {
            savedStatus = status;
        }
        
        tmpDataObjInfo = tmpDataObjInfo->next;
    }

    return (savedStatus);
} 

int
syncDataObjPhyPathS (rsComm_t *rsComm, dataObjInp_t *dataObjInp,
                     dataObjInfo_t *dataObjInfo, char *acLCollection)
{
    int status, status1;
    fileRenameInp_t fileRenameInp;
    rescInfo_t *rescInfo = 0;
    modDataObjMeta_t modDataObjMetaInp;
    keyValPair_t regParam;
    vaultPathPolicy_t vaultPathPolicy;

    if (strcmp (dataObjInfo->rescInfo->rescName, BUNDLE_RESC) == 0)
        return 0;

    int create_path = 0;
    eirods::error err = eirods::get_resource_property< int >( dataObjInfo->rescInfo->rescName, "create_path", create_path );
    if( !err.ok() ) {
        eirods::log( PASS( false, -1, "syncDataObjPhyPathS - failed.", err ) );
    }

    // JMC - legacy code - if (RescTypeDef[dataObjInfo->rescInfo->rescTypeInx].createPathFlag == NO_CREATE_PATH) {
    if( NO_CREATE_PATH == create_path ) {  
        /* no need to sync for path created by resource */
        return 0;
    }

    status = getVaultPathPolicy (rsComm, dataObjInfo, &vaultPathPolicy);
    if (status < 0) {
        rodsLog (LOG_NOTICE,
                 "syncDataObjPhyPathS: getVaultPathPolicy error for %s, status = %d",
                 dataObjInfo->objPath, status);
    } else {
        if (vaultPathPolicy.scheme != GRAFT_PATH_S) {
            /* no need to sync */
            return (0);
        }
    }

    if (isInVault (dataObjInfo) == 0) {
        /* not in vault. */
        return (0);
    }

    if (dataObjInfo->rescInfo->rescStatus == INT_RESC_STATUS_DOWN)
        return SYS_RESC_IS_DOWN;

    /* Save the current objPath */
    memset (&fileRenameInp, 0, sizeof (fileRenameInp));
    rstrcpy (fileRenameInp.oldFileName, dataObjInfo->filePath, MAX_NAME_LEN);
    rstrcpy (fileRenameInp.rescHier, dataObjInfo->rescHier, MAX_NAME_LEN);
    rstrcpy(fileRenameInp.objPath, dataObjInfo->objPath, MAX_NAME_LEN);
    if (dataObjInp == NULL) {
        dataObjInp_t myDdataObjInp;
        memset (&myDdataObjInp, 0, sizeof (myDdataObjInp));
        rstrcpy (myDdataObjInp.objPath, dataObjInfo->objPath, MAX_NAME_LEN);
        status = getFilePathName (rsComm, dataObjInfo, &myDdataObjInp);
    } else {
        status = getFilePathName (rsComm, dataObjInfo, dataObjInp);
    }

    if (strcmp (fileRenameInp.oldFileName, dataObjInfo->filePath) == 0) {
        return (0);
    }

    rescInfo = dataObjInfo->rescInfo;
    /* see if the new file exist */
    if (getSizeInVault (rsComm, dataObjInfo) >= 0) {
        if (chkAndHandleOrphanFile (rsComm, dataObjInfo->objPath, dataObjInfo->rescHier,
                                    dataObjInfo->filePath, rescInfo, OLD_COPY) <= 0) {
            rodsLog (LOG_ERROR,
                     "syncDataObjPhyPath:newFileName %s already in use",
                     dataObjInfo->filePath);
            return (SYS_PHY_PATH_INUSE);
        }
    }

    /* rename it */
    // JMC - legacy resource - fileRenameInp.fileType = (fileDriverType_t)RescTypeDef[rescTypeInx].driverType;
    rstrcpy (fileRenameInp.addr.hostAddr, rescInfo->rescLoc, NAME_LEN);
    rstrcpy (fileRenameInp.newFileName, dataObjInfo->filePath,
             MAX_NAME_LEN);
    status = rsFileRename (rsComm, &fileRenameInp);
    if (status < 0) {
        rodsLog (LOG_ERROR,
                 "syncDataObjPhyPath:rsFileRename from %s to %s failed,status=%d",
                 fileRenameInp.oldFileName, fileRenameInp.newFileName, status);
        return (status);
    }

    /* register the change */
    memset (&regParam, 0, sizeof (regParam));
    addKeyVal (&regParam, FILE_PATH_KW, fileRenameInp.newFileName);
    if (acLCollection != NULL)
        addKeyVal (&regParam, ACL_COLLECTION_KW, acLCollection);
    modDataObjMetaInp.dataObjInfo = dataObjInfo;
    modDataObjMetaInp.regParam = &regParam;
    status = rsModDataObjMeta (rsComm, &modDataObjMetaInp);
    clearKeyVal (&regParam);
    if (status < 0) {
        char tmpPath[MAX_NAME_LEN]; 
        rodsLog (LOG_ERROR,
                 "syncDataObjPhyPath: rsModDataObjMeta of %s error. stat = %d",
                 fileRenameInp.newFileName, status);
        /* need to rollback the change in path */
        rstrcpy (tmpPath, fileRenameInp.oldFileName, MAX_NAME_LEN);
        rstrcpy (fileRenameInp.oldFileName, fileRenameInp.newFileName,
                 MAX_NAME_LEN);
        rstrcpy (fileRenameInp.newFileName, tmpPath, MAX_NAME_LEN);
        status1 = rsFileRename (rsComm, &fileRenameInp);

        if (status1 < 0) {
            rodsLog (LOG_ERROR,
                     "syncDataObjPhyPath: rollback rename %s failed, status = %d",
                     fileRenameInp.oldFileName, status1);
        }
        return (status);
    }
    return (0);
}

/* syncCollPhyPath - sync the path of the phy path with the path of
 * the data ovject in the new collection. This is unsed by rename to sync 
 * the path of the phy path with the new path.
 */

int
syncCollPhyPath (rsComm_t *rsComm, char *collection)
{
    int status, i;
    int savedStatus = 0;
    genQueryOut_t *genQueryOut = NULL;
    genQueryInp_t genQueryInp;
    int continueInx;

    status = rsQueryDataObjInCollReCur (rsComm, collection, 
                                        &genQueryInp, &genQueryOut, NULL, 0);

    if (status<0 && status != CAT_NO_ROWS_FOUND) {
        savedStatus=status; /* return the error code */
    }

    while (status >= 0) {
        sqlResult_t *dataIdRes, *subCollRes, *dataNameRes, *replNumRes, 
            *rescNameRes, *filePathRes, *rescHierRes;
        char *tmpDataId, *tmpDataName, *tmpSubColl, *tmpReplNum, 
            *tmpRescName, *tmpFilePath, *tmpRescHier;
        dataObjInfo_t dataObjInfo;

        memset (&dataObjInfo, 0, sizeof (dataObjInfo));

        if ((dataIdRes = getSqlResultByInx (genQueryOut, COL_D_DATA_ID))
            == NULL) {
            rodsLog (LOG_ERROR,
                     "syncCollPhyPath: getSqlResultByInx for COL_COLL_NAME failed");
            return (UNMATCHED_KEY_OR_INDEX);
        }
        if ((subCollRes = getSqlResultByInx (genQueryOut, COL_COLL_NAME))
            == NULL) {
            rodsLog (LOG_ERROR,
                     "syncCollPhyPath: getSqlResultByInx for COL_COLL_NAME failed");
            return (UNMATCHED_KEY_OR_INDEX);
        }
        if ((dataNameRes = getSqlResultByInx (genQueryOut, COL_DATA_NAME))
            == NULL) {
            rodsLog (LOG_ERROR,
                     "syncCollPhyPath: getSqlResultByInx for COL_DATA_NAME failed");
            return (UNMATCHED_KEY_OR_INDEX);
        }
        if ((replNumRes = getSqlResultByInx (genQueryOut, COL_DATA_REPL_NUM))
            == NULL) {
            rodsLog (LOG_ERROR,
                     "syncCollPhyPath:getSqlResultByIn for COL_DATA_REPL_NUM failed");
            return (UNMATCHED_KEY_OR_INDEX);
        }
        if ((rescNameRes = getSqlResultByInx (genQueryOut, COL_D_RESC_NAME))
            == NULL) {
            rodsLog (LOG_ERROR,
                     "syncCollPhyPath: getSqlResultByInx for COL_D_RESC_NAME failed");
            return (UNMATCHED_KEY_OR_INDEX);
        }
        if ((filePathRes = getSqlResultByInx (genQueryOut, COL_D_DATA_PATH))
            == NULL) {
            rodsLog (LOG_ERROR,
                     "syncCollPhyPath: getSqlResultByInx for COL_D_DATA_PATH failed");
            return (UNMATCHED_KEY_OR_INDEX);
        }
        if ((rescHierRes = getSqlResultByInx (genQueryOut, COL_D_RESC_HIER))
            == NULL) {
            rodsLog (LOG_ERROR,
                     "syncCollPhyPath: getSqlResultByInx for COL_D_RESC_HIER failed");
            return (UNMATCHED_KEY_OR_INDEX);
        }

        for (i = 0;i < genQueryOut->rowCnt; i++) {
            tmpDataId = &dataIdRes->value[dataIdRes->len * i];
            tmpDataName = &dataNameRes->value[dataNameRes->len * i];
            tmpSubColl = &subCollRes->value[subCollRes->len * i];
            tmpReplNum = &replNumRes->value[replNumRes->len * i];
            tmpRescName = &rescNameRes->value[rescNameRes->len * i];
            tmpFilePath = &filePathRes->value[filePathRes->len * i];
            tmpRescHier = &rescHierRes->value[rescHierRes->len * i];

            dataObjInfo.dataId = strtoll (tmpDataId, 0, 0);
            snprintf (dataObjInfo.objPath, MAX_NAME_LEN, "%s/%s",
                      tmpSubColl, tmpDataName);
            dataObjInfo.replNum = atoi (tmpReplNum);
            rstrcpy (dataObjInfo.rescName, tmpRescName, NAME_LEN);
            rstrcpy (dataObjInfo.rescHier, tmpRescHier, MAX_NAME_LEN);
            /*status = resolveResc (tmpRescName, &dataObjInfo.rescInfo);
              if (status < 0) {
              rodsLog( LOG_ERROR,"syncCollPhyPath: resolveResc error for %s, status = %d",
              tmpRescName, status);
              return (status);
              }*/
           
            dataObjInfo.rescInfo = new rescInfo_t; 
            eirods::error err = eirods::get_resc_info( tmpRescName, *dataObjInfo.rescInfo );
            if( !err.ok() ) {
                std::stringstream msg;
                msg << "getDefaultLocalRescInfo - failed to get resource info";
                msg << tmpRescName;
                eirods::log( PASS( false, -1, msg.str(), err ) );
            }

            rstrcpy (dataObjInfo.filePath, tmpFilePath, MAX_NAME_LEN);

            status = syncDataObjPhyPathS (rsComm, NULL, &dataObjInfo,
                                          collection);
            if (status < 0) {
                rodsLog (LOG_ERROR,
                         "syncCollPhyPath: syncDataObjPhyPathS error for %s,stat=%d",
                         dataObjInfo.filePath, status);
                savedStatus = status;
            }

        }

        continueInx = genQueryOut->continueInx;

        freeGenQueryOut (&genQueryOut);

        if (continueInx > 0) {
            /* More to come */
            genQueryInp.continueInx = continueInx;
            status =  rsGenQuery (rsComm, &genQueryInp, &genQueryOut);
        } else {
            break;
        }
    }
    clearGenQueryInp (&genQueryInp);

    return (savedStatus);
}

int
isInVault (dataObjInfo_t *dataObjInfo)
{
    int len;

    if (dataObjInfo == NULL || dataObjInfo->rescInfo == NULL) {
        return (SYS_INTERNAL_NULL_INPUT_ERR);
    }

    len = strlen (dataObjInfo->rescInfo->rescVaultPath);

    if (strncmp (dataObjInfo->rescInfo->rescVaultPath, 
                 dataObjInfo->filePath, len) == 0) {
        return (1);
    } else {
        return (0);
    }
}

#if 0 // JMC - UNUSED
/* initStructFileOprInp - initialize the structFileOprInp struct for
 * UNUSED :: rsStructFileBundle and rsStructFileExtAndReg
 */

int
initStructFileOprInp (rsComm_t *rsComm, 
                      structFileOprInp_t *structFileOprInp,
                      structFileExtAndRegInp_t *structFileExtAndRegInp, 
                      dataObjInfo_t *dataObjInfo)
{
    int status;
    vaultPathPolicy_t vaultPathPolicy;
    int addUserNameFlag;

    memset (structFileOprInp, 0, sizeof (structFileOprInp_t));
    structFileOprInp->specColl = (specColl_t*)malloc (sizeof (specColl_t));
    memset (structFileOprInp->specColl, 0, sizeof (specColl_t));
    if( strcmp (dataObjInfo->dataType, TAR_DT_STR) == 0 ||
        strstr (dataObjInfo->dataType, BUNDLE_STR) == 0) { // JMC - backport 4658
        structFileOprInp->specColl->type = TAR_STRUCT_FILE_T;
    } else if (strcmp (dataObjInfo->dataType, HAAW_DT_STR) == 0) {
        structFileOprInp->specColl->type = HAAW_STRUCT_FILE_T;
    } else {
        rodsLog (LOG_ERROR,
                 "initStructFileOprInp: objType %s of %s is not a struct file",
                 dataObjInfo->dataType, dataObjInfo->objPath);
        return SYS_OBJ_TYPE_NOT_STRUCT_FILE;
    }

    rstrcpy (structFileOprInp->specColl->collection,
             structFileExtAndRegInp->collection, MAX_NAME_LEN);
    rstrcpy (structFileOprInp->specColl->objPath,
             structFileExtAndRegInp->objPath, MAX_NAME_LEN);
    structFileOprInp->specColl->collClass = STRUCT_FILE_COLL;
    rstrcpy (structFileOprInp->specColl->resource, dataObjInfo->rescName,
             NAME_LEN);
    rstrcpy (structFileOprInp->specColl->phyPath,
             dataObjInfo->filePath, MAX_NAME_LEN);
    rstrcpy (structFileOprInp->addr.hostAddr, dataObjInfo->rescInfo->rescLoc,
             NAME_LEN);
    /* set the cacheDir */
    status = getVaultPathPolicy (rsComm, dataObjInfo, &vaultPathPolicy);
    if (status < 0) {
        return (status);
    }
    /* don't do other type of Policy except GRAFT_PATH_S */
    if (vaultPathPolicy.scheme == GRAFT_PATH_S) {
        addUserNameFlag = vaultPathPolicy.addUserName;
    } else {
        rodsLog (LOG_ERROR,
                 "initStructFileOprInp: vaultPathPolicy.scheme %d for resource %s is not GRAFT_PATH_S",
                 vaultPathPolicy.scheme, structFileOprInp->specColl->resource);
        return SYS_WRONG_RESC_POLICY_FOR_BUN_OPR;
    }
    status = setPathForGraftPathScheme (structFileExtAndRegInp->collection,
                                        dataObjInfo->rescInfo->rescVaultPath, addUserNameFlag,
                                        rsComm->clientUser.userName, vaultPathPolicy.trimDirCnt,
                                        structFileOprInp->specColl->cacheDir);

    return (status);
}
#endif // JMC - UNUSED

int
getDefFileMode ()
{
    int defFileMode;
    char *modeStr; // JMC - backport 4841
    if ((modeStr = getenv ("DefFileMode")) != NULL && *modeStr == '0') { // JMC - backport 4841
        defFileMode = strtol (getenv ("DefFileMode"), 0, 0);
    } else {
        defFileMode = DEFAULT_FILE_MODE;
    }
    return defFileMode;
}

int
getDefDirMode ()
{
    int defDirMode;
    char *modeStr; // JMC - backport 4841
    if ((modeStr = getenv ("DefDirMode")) != NULL && *modeStr == '0') { // JMC - backport 4841
        defDirMode = strtol (getenv ("DefDirMode"), 0, 0);
    } else {
        defDirMode = DEFAULT_DIR_MODE;
    }
    return defDirMode;
}

int
getLogPathFromPhyPath (char *phyPath, rescInfo_t *rescInfo, char *outLogPath)
{
    int len;
    char *tmpPtr;
    zoneInfo_t *tmpZoneInfo = NULL;
    int status;

    if (phyPath == NULL || rescInfo == NULL || outLogPath == NULL)
        return USER__NULL_INPUT_ERR;

    len = strlen (rescInfo->rescVaultPath);
    if (strncmp (rescInfo->rescVaultPath, phyPath, len) != 0) return -1;
    tmpPtr = phyPath + len;

    if (*tmpPtr != '/') return -1;

    tmpPtr ++;
    status = getLocalZoneInfo (&tmpZoneInfo);
    if (status < 0 || NULL == tmpZoneInfo ) return status; // JMC cppcheck - nullptr

    len = strlen (tmpZoneInfo->zoneName);   
    if (strncmp (tmpZoneInfo->zoneName, tmpPtr, len) == 0 &&
        *(tmpPtr + len) == '/') {
        /* start with zoneName */
        tmpPtr += (len + 1);
    }

    snprintf (outLogPath, MAX_NAME_LEN, "/%s/%s", tmpZoneInfo->zoneName,
              tmpPtr);

    return 0;
}

/* rsMkOrphanPath - given objPath, compose the orphan path which is
 * /myZone/trash/orphan/userName#userZone/filename.random
 * Also make the required directory.
 */
int
rsMkOrphanPath (rsComm_t *rsComm, char *objPath, char *orphanPath)
{
    int status;
    char *tmpStr;
    char *orphanPathPtr;
    int len;
    char parentColl[MAX_NAME_LEN], childName[MAX_NAME_LEN];
    collInp_t collCreateInp;

    status = splitPathByKey(objPath, parentColl, childName, '/');

    if (status < 0) {
        rodsLog (LOG_ERROR,
                 "rsMkOrphanPath: splitPathByKey error for %s, status = %d",
                 objPath, status);
        return (status);
    }
    orphanPathPtr = orphanPath;
    *orphanPathPtr = '/';
    orphanPathPtr++;
    tmpStr = objPath + 1;
    /* copy the zone */
    while (*tmpStr != '\0') {
        *orphanPathPtr = *tmpStr;
        orphanPathPtr ++;
        len ++;
        if (*tmpStr == '/') {
            tmpStr ++;
            break;
        }
        tmpStr ++;
    }
    len = strlen (rsComm->clientUser.userName) + 
        strlen (rsComm->clientUser.rodsZone);
    snprintf (orphanPathPtr, len + 20, "trash/orphan/%s#%s",
              rsComm->clientUser.userName, rsComm->clientUser.rodsZone);

    memset (&collCreateInp, 0, sizeof (collCreateInp));
    rstrcpy (collCreateInp.collName, orphanPath, MAX_NAME_LEN);
    status = rsCollCreate (rsComm, &collCreateInp);

    if (status < 0 && status != CAT_NAME_EXISTS_AS_COLLECTION) {
        rodsLogError (LOG_ERROR, status,
                      "rsMkOrphanPath: rsCollCreate error for %s",
                      orphanPath);
    }
    orphanPathPtr = orphanPath + strlen (orphanPath); 

    snprintf (orphanPathPtr, strlen (childName) + 20, "/%s.%-d",
              childName, (uint) random());

    return 0;
}

// =-=-=-=-=-=-=-
// JMC - backport 4598
int
getDataObjLockPath (char *objPath, char **outLockPath)
{
    int i;
    char *objPathPtr, *tmpPtr;
    char tmpPath[MAX_NAME_LEN];
    int c;
    int len;

    if (objPath == NULL || outLockPath == NULL) return USER__NULL_INPUT_ERR;
    objPathPtr = objPath; // JMC - backport 4604

    /* skip over the first 3 '/' */
    for (i = 0; i < 3; i++) {
        tmpPtr = strchr (objPathPtr, '/');
        if (tmpPtr == NULL) {
            break;      /* just use the shorten one */
        } else {
            /* skip over '/' */
            objPathPtr = tmpPtr + 1;
        }
    }
    rstrcpy (tmpPath, objPathPtr, MAX_NAME_LEN);
    /* replace all '/' with '.' */
    objPathPtr = tmpPath;

    while ((c = *objPathPtr) != '\0') {
        if (c == '/')
            *objPathPtr = '.';
        objPathPtr++;
    }

    len = strlen (getConfigDir()) + strlen (LOCK_FILE_DIR) + 
        strlen (tmpPath) + strlen (LOCK_FILE_TRAILER) + 10; // JMC - backport 6404
    *outLockPath = (char *) malloc (len); 

    snprintf (*outLockPath, len, "%-s/%-s/%-s.%-s", getConfigDir(),  // JMC - backport 6404
              LOCK_FILE_DIR, tmpPath, LOCK_FILE_TRAILER);

    return 0;
}

/* fsDataObjLock - lock the data object using the local file system
 * Input:
 *    char *objPath - The full Object path
 *    int cmd - the fcntl cmd - valid values are F_SETLK, F_SETLKW and F_GETLK
 *    int type - the fcntl type - valid values are F_UNLCK, F_WRLCK, F_RDLCK
 *    int infd - For F_UNLCK, the fd of the file to unlock
 */

int
fsDataObjLock (char *objPath, int cmd, int type, int infd)
{
    int status;
    int myFd;
    struct flock myflock;
    char *path = NULL;

    if (type != F_UNLCK) {
        if ((status = getDataObjLockPath (objPath, &path)) < 0) {
            rodsLogError (LOG_ERROR, status,
                          "fsDataObjLock: getDataObjLockPath error for %s", objPath);
            return status;
        }
        myFd = open (path, O_RDWR | O_CREAT, 0644);
        if (myFd < 0) {
            status = FILE_OPEN_ERR - errno;
            rodsLogError (LOG_ERROR, status,
                          "fsDataObjLock: open error for %s", objPath);
            return status;
        }
    } else {
        myFd = infd;
    }
#ifndef _WIN32
    bzero (&myflock, sizeof (myflock));
    myflock.l_type = type;
    myflock.l_whence = SEEK_SET;
    status = fcntl (myFd, cmd, &myflock);
    if (status < 0) {
        /* this is not necessary an error. cmd F_SETLK or F_GETLK can return 
         * -1. */
        status = SYS_FS_LOCK_ERR - errno;
        rodsLogError( LOG_DEBUG, status,"fsDataObjLock: fcntl error for %s, cmd = %d, type = %d", 
                      objPath, cmd, type); // JMC - backport 4604

        if (path != NULL) free (path); // JMC - backport 4604

        close (myFd);
        return (status);
    }
#endif
    if (path != NULL) free (path); // JMC - backport 4604
    if (type == F_UNLCK) {
        close (myFd);
        myFd = 0;
    } else if (cmd == F_GETLK) {
        /* get the status of locking the file. return F_UNLCK if no conflict */
        close (myFd);
        myFd = myflock.l_type;
    }
    return (myFd);
}

int
getLeafRescPathName(
    const std::string& _resc_hier,
    std::string& _ret_string)
{
    int result = 0;
    eirods::hierarchy_parser hp;
    eirods::error ret;
    ret = hp.set_string(_resc_hier);
    if(!ret.ok()) {
        std::stringstream msg;
        msg << "Unable to parse hierarchy string: \"" << _resc_hier << "\"";
        eirods::log(LOG_ERROR, msg.str());
        result = ret.code();
    } else {
        std::string leaf;
        ret = hp.last_resc(leaf);
        if(!ret.ok()) {
            std::stringstream msg;
            msg << "Unable to retrieve last resource from hierarchy string: \"" << _resc_hier << "\"";
            eirods::log(LOG_ERROR, msg.str());
            result = ret.code();
        } else {
            ret = eirods::get_resource_property<std::string>(leaf, "path", _ret_string);
            if(!ret.ok()) {
                std::stringstream msg;
                msg << "Unable to get vault path from resource: \"" << leaf << "\"";
                eirods::log(LOG_ERROR, msg.str());
                result = ret.code();
            }
        }
    }
    return result;
}

// =-=-=-=-=-=-=-

