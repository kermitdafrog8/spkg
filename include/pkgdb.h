/*----------------------------------------------------------------------*\
|* spkg - The Unofficial Slackware Linux Package Manager                *|
|*                                      designed by Ond�ej Jirman, 2005 *|
|*----------------------------------------------------------------------*|
|*          No copy/usage restrictions are imposed on anybody.          *|
\*----------------------------------------------------------------------*/
/** @defgroup pkgdb_api Package Database API

\ref pkgdb_api consists of three groups of functions:

\section funcs1 Database manipulation functions
\li \ref db_open and \ref db_close
\li \ref db_sync_from_legacydb and \ref db_sync_to_legacydb

\section funcs2 Package manipulation functions
\li \ref db_get_pkg, \ref db_add_pkg and \ref db_rem_pkg
\li \ref db_legacy_get_pkg, \ref db_legacy_add_pkg and \ref db_legacy_rem_pkg

\section funcs3 Packages list query functions
These functions returns list of package names.
\li \ref db_query and \ref db_legacy_query

*//*--------------------------------------------------------------------*/
/** @addtogroup pkgdb_api */
/*! @{ */

#ifndef SPKG__PKGDB_H
#define SPKG__PKGDB_H

#include <glib.h>
#include <time.h>
#include "error.h"
#include "filedb.h"

/** Directory where the package database is stored. */
#define PKGDB_DIR "var/log"

#define DB_NOPEN   E(0) /**< database is not open */
#define DB_OPEN    E(1) /**< database is already open (when opening) */
#define DB_CORRUPT E(2) /**< database is corrupted */
#define DB_NOTEX   E(3) /**< package not exist in database */
#define DB_EXIST   E(4) /**< package already in database */
#define DB_SQL     E(5) /**< sqlite error */
#define DB_BLOCKED E(6) /**< db is open by another proccess */

/** What to get when getting package from database. */
typedef enum {
  DB_GET_FULL,         /**< get everything */
  DB_GET_WITHOUT_FILES /**< get basic info (everyhing except file list and doinst.sh) */
} db_get_type;

/** What to get when querying package list from database. */
typedef enum {
  DB_QUERY_PKGS_WITH_FILES,    /**< query list of actual packages with files */
  DB_QUERY_PKGS_WITHOUT_FILES, /**< query list of actual packages without files */
  DB_QUERY_NAMES               /**< query list of package names */
} db_query_type;

/** File information structure. 
 *
 * @li N - no touch: user should not modify such variable
 * @li R - required: user must set such variable
 * @li O - optional: user may set such variable
 * @li A - automat: automatically set variable
 */
struct db_file {
  guint32 id;   /**< unique file id asigned by filedb [N] */
  guint16 refs; /**< reference count of the file (by how many packages file is referenced) [N] */
  gchar* path;  /**< relative path to the file (relative to the root) [R] */
  gchar* link;  /**< path to the link target (if file is symlink or link) [O] */
};

/** Package information structure. */
struct db_pkg {
  /* name */
  gchar* name;       /**< full name of the package [R] */
  gchar* shortname;  /**< short name of the package [NA] */
  gchar* version;    /**< version of the package [NA] */
  gchar* arch;       /**< architecture of the package [NA] */
  gchar* build;      /**< build of the package [NA] */

  /* details */
  time_t time;       /**< package installation/upgrade time [NA] */
  gsize  csize;      /**< compressed size of the package in kB [O] */
  gsize  usize;      /**< uncompressed size of the package in kB [O] */
  gchar* location;   /**< original package location (path) [O] */
  gchar* desc;       /**< cleaned (i.e. without rubbish :) package description [O] */
  gchar* doinst;     /**< complete doinst.sh script contents [O] */
  
  GSList* files;     /**< list of the files in the package [O] */

  /* internals */
  gint id;           /**< unique package id assigned by pkgdb [N] */
};

/** Open package database.
 *
 * @param root root directory
 * @param e Error object.
 * @return 0 on success, 1 on error
 */
extern gint db_open(const gchar* root, struct error* e);

/** Close package database. 
 */
extern void db_close();

/** Create package object.
 *
 * Package name is checked and automatically parsed into parts.
 *
 * @param name Package name (something like: blah-1.0-i486-1)
 * @return 0 if invalid name, \ref db_pkg object on success
 */
extern struct db_pkg* db_alloc_pkg(gchar* name);

/** Free \ref db_pkg object.
 *
 * @param pkg \ref db_pkg object
 */
extern void db_free_pkg(struct db_pkg* pkg);

/** Create file object.
 *
 * @param path File path. (it is not strduped)
 * @param link File link (if it is symlink). (it is not strduped)
 * @return \ref db_file object on success
 */
extern struct db_file* db_alloc_file(gchar* path, gchar* link);

/** Add package to the database.
 *
 * @param pkg \ref db_pkg object
 * @return 0 on success, 1 on error
 */
extern gint db_add_pkg(struct db_pkg* pkg);

/** Get package from the database.
 *
 * @param name Package name (something like: blah-1.0-i486-1)
 * @param type What to get. See \ref db_get_type.
 * @return 0 if not found, \ref db_pkg object on success
 */
extern struct db_pkg* db_get_pkg(gchar* name, db_get_type type);

/** Remove package from the database.
 *
 * @param name Package name (something like: blah-1.0-i486-1)
 * @return 0 on success, 1 on error
 */
extern gint db_rem_pkg(gchar* name);

/** Add package to the legacy database.
 *
 * @param pkg \ref db_pkg object
 * @return 0 on success, 1 on error
 */
extern gint db_legacy_add_pkg(struct db_pkg* pkg);

/** Get package from legacy database.
 *
 * @param name Package name (something like: blah-1.0-i486-1)
 * @param type What to get. See \ref db_get_type.
 * @return 0 if not found, \ref db_pkg object on success
 */
extern struct db_pkg* db_legacy_get_pkg(gchar* name, db_get_type type);

/** Remove package from legacy database.
 *
 * @param name Package name (something like: blah-1.0-i486-1)
 * @return 0 on success, 1 on error
 */
extern gint db_legacy_rem_pkg(gchar* name);

/** Package selector callback function.
 *
 * @param pkg filled package structure
 * @param data arbitrary data passed from the \ref db_query function.
 * @return 1 if package should be selected, 0 otherwise, -1 on error.
 */
typedef gint (*db_selector)(const struct db_pkg* pkg, const void* data);

/** Get packages list from the database.
 *
 * @param cb package selector callback function
 * @param data arbitrary data passed to the package selector function
 * @param type format of result. see \ref db_query_type
 * @return list of package names, 0 if empty or error
 */
extern GSList* db_query(db_selector cb, const void* data, db_query_type type);

/** Get packages list from the legacy database.
 *
 * @param cb package selector callback function
 * @param data arbitrary data passed to the package selector function
 * @param type format of result. see \ref db_query_type
 * @return list of package names, 0 if empty or error
 */
extern GSList* db_legacy_query(db_selector cb, const void* data, db_query_type type);

/** Free packages list returned by \ref db_query().
 *
 * @param pkgs list of package names returned by \ref db_query()
 * @param type must be the same as when the query was called. see \ref db_query_type
 */
extern void db_free_query(GSList* pkgs, db_query_type type);

/** Synchronize legacy databse with spkg database.
 *
 * @return 0 on success, 1 on error
 */
extern gint db_sync_to_legacydb();

/** Synchronize spkg databse with legacy database.
 *
 * @return 0 on success, 1 on error
 */
extern gint db_sync_from_legacydb();
 
/** Get fdb object.
 *
 * @return fdb object, 0 on error
 */
extern struct fdb* db_get_fdb();

#endif

/*! @} */
