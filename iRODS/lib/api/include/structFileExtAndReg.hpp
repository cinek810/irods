/*** Copyright (c), The Regents of the University of California            ***
 *** For more information please refer to files in the COPYRIGHT directory ***/
/* structFileExtAndReg.h - This file may be generated by a program or
 * script
 */

#ifndef STRUCT_FILE_EXT_AND_REG_HPP
#define STRUCT_FILE_EXT_AND_REG_HPP

/* This is a Object File I/O call */

#include "rods.hpp"
#include "rcMisc.hpp"
#include "procApiRequest.hpp"
#include "apiNumber.hpp"
#include "initServer.hpp"

typedef struct StructFileExtAndRegInp {
    char objPath[MAX_NAME_LEN];		/* the obj path of the struct file */
    char collection[MAX_NAME_LEN];	/* the collection under which the
					 * extracted files are registered.
					 */
    int oprType;       /* see syncMountedColl.h. valid oprType are  // JMC - backport 4643
                        * CREATE_TAR_OPR and ADD_TO_TAR_OPR */

    int flags;				/* not used */
    keyValPair_t condInput;   /* include chksum flag and value */
} structFileExtAndRegInp_t;

#define StructFileExtAndRegInp_PI "str objPath[MAX_NAME_LEN]; str collection[MAX_NAME_LEN]; int oprType; int flags; struct KeyValPair_PI;"
#ifdef __cplusplus
extern "C" {
#endif

#if defined(RODS_SERVER)
#define RS_STRUCT_FILE_EXT_AND_REG rsStructFileExtAndReg
/* prototype for the server handler */
int
rsStructFileExtAndReg( rsComm_t *rsComm,
                       structFileExtAndRegInp_t *structFileExtAndRegInp );
int
chkCollForExtAndReg( rsComm_t *rsComm, char *collection,
                     rodsObjStat_t **rodsObjStatOut );
int
regUnbunSubfiles( rsComm_t *rsComm, const char *_resc_name, const char* rescHier,
                  char *collection, char *phyBunDir, int flags, genQueryOut_t *attriArray );
int
regSubfile( rsComm_t *rsComm, const char *_resc_name, const char* rescHier,
            char *subObjPath, char *subfilePath, rodsLong_t dataSize, int flags );
int
addRenamedPhyFile( char *subObjPath, char *oldFileName, char *newFileName,
                   renamedPhyFiles_t *renamedPhyFiles );
int
postProcRenamedPhyFiles( renamedPhyFiles_t *renamedPhyFiles, int regStatus );
int
cleanupBulkRegFiles( rsComm_t *rsComm, genQueryOut_t *bulkDataObjRegInp );
int
postProcBulkPut( rsComm_t *rsComm, genQueryOut_t *bulkDataObjRegInp,
                 genQueryOut_t *bulkDataObjRegOut );
#else
#define RS_STRUCT_FILE_EXT_AND_REG NULL
#endif

/* prototype for the client call */
int
rcStructFileExtAndReg( rcComm_t *conn,
                       structFileExtAndRegInp_t *structFileExtAndRegInp );

#ifdef __cplusplus
}
#endif

#endif	/* STRUCT_FILE_EXT_AND_REG_H */
