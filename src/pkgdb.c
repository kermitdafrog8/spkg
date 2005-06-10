/*----------------------------------------------------------------------*\
|* spkg - The Unofficial Slackware Linux Package Manager                *|
|*                                      designed by Ond�ej Jirman, 2005 *|
|*----------------------------------------------------------------------*|
|*          No copy/usage restrictions are imposed on anybody.          *|
\*----------------------------------------------------------------------*/
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <regex.h>
#include <string.h>

#include "sql.h"
#include "sys.h"
#include "pkgname.h"
#include "filedb.h"

#include "pkgdb.h"

#include "bench.h"

/* private 
 ************************************************************************/

static gboolean _db_is_open = 0;
static gchar* _db_topdir = 0;
static gchar* _db_dbfile = 0;
static gchar* _db_dbroot = 0;
static gchar* _db_errstr = 0;
static gint   _db_errno = 0;

static __inline__ void _db_reset_error()
{
  if (G_UNLIKELY(_db_errstr != 0))
  {
    g_free(_db_errstr);
    _db_errstr = 0;
  }
  _db_errno = DB_OK;
}

#define _db_open_check(v) \
  if (!_db_is_open) \
  { \
    _db_set_error(DB_CLOSED, "trying to access closed package database"); \
    return v; \
  }

static void _db_set_error(gint errno, const gchar* fmt, ...)
{
  va_list ap;
  _db_reset_error();
  _db_errno = errno;
  va_start(ap, fmt);
  _db_errstr = g_strdup_vprintf(fmt, ap);
  va_end(ap);
  _db_errstr = g_strdup_printf("error[pkgdb]: %s", _db_errstr);
}

/* public 
 ************************************************************************/

gint db_open(const gchar* root)
{
  gchar** d;
  gchar* checkdirs[] = {
    "packages", "scripts", "removed_packages", "removed_scripts", "setup", 
    "spkgdb", 0
  };
  
  _db_reset_error();
  if (_db_is_open)
  {
    _db_set_error(DB_OPEN, "can't open package database (it is already open)");
    return 1;
  }
  
  if (root == 0)
    root = "";

  _db_topdir = g_strdup_printf("%s/%s", root, PKGDB_DIR);
  /* check legacy and spkg db dirs */
  for (d = checkdirs; *d != 0; d++)
  {
    gchar* tmpdir = g_strdup_printf("%s/%s", _db_topdir, *d);
    /* if it is not a directory, clean it and create it */
    if (sys_file_type(tmpdir,1) != SYS_DIR)
    {
      sys_rm_rf(tmpdir);
      sys_mkdir_p(tmpdir);
      chmod(tmpdir, 0755);
      /* if it is still not a directory, return with error */
      if (sys_file_type(tmpdir,1) != SYS_DIR)
      {
        _db_set_error(DB_OTHER, "can't open package database (%s should be an accessible directory)", tmpdir);
        g_free(tmpdir);
        goto err0;
      }
    }
    g_free(tmpdir);
  }

  /* check spkg db file */
  _db_dbroot = g_strdup_printf("%s/%s", _db_topdir, "spkgdb");
  _db_dbfile = g_strdup_printf("%s/%s", _db_dbroot, "spkg.db");
  if (sys_file_type(_db_dbfile,0) != SYS_REG && sys_file_type(_db_dbfile,0) != SYS_NONE)
  {
    _db_set_error(DB_OTHER, "can't open package database (%s is not accessible)", _db_dbfile);
    goto err1;
  }

  if (fdb_open(_db_dbroot))
  {
    _db_set_error(DB_OTHER, "can't open file database\n%s", fdb_error());
    goto err1;
  }

  /* setup sql error handling */
  sql_push_context(SQL_ERRJUMP,0);
  if (setjmp(sql_errjmp) == 1)
  { /* sql exception occured */
    _db_set_error(DB_OTHER, "can't open package database (sql error)\n%s", sql_error());
    goto err2;
  }

  /* open sql database */
  sql_open(_db_dbfile);
  sql_exec("PRAGMA temp_store = MEMORY;");
  sql_exec("PRAGMA synchronous = OFF;");
  sql_transaction_begin();

  /* if package table does not exist create it */
  if (!sql_table_exist("packages"))
  {
    sql_exec(
      "CREATE TABLE packages ("
      " id INTEGER PRIMARY KEY,"
      " name TEXT UNIQUE NOT NULL,"
      " shortname TEXT NOT NULL,"
      " version TEXT NOT NULL,"
      " arch TEXT NOT NULL,"
      " build TEXT NOT NULL,"
      " csize INTEGER,"
      " usize INTEGER,"
      " desc TEXT,"
      " location TEXT,"
      " files BLOB "
      ");"
    );
  }

  sql_pop_context(1);
  _db_is_open = 1;
  return 0;

 err2:
  sql_close();
 err1:
  g_free(_db_dbfile);
  g_free(_db_dbroot);
 err0:
  g_free(_db_topdir);
  return 1;
}

void db_close()
{
  _db_reset_error();
  fdb_close();
  sql_close();
  g_free(_db_dbfile);
  g_free(_db_topdir);
  g_free(_db_dbroot);
  g_blow_chunks();
  _db_is_open = 0;
}

gint db_errno()
{
  return _db_errno;
}

gchar* db_error()
{
  return _db_errstr;
}

struct db_pkg* db_alloc_pkg(gchar* name)
{
  struct db_pkg* p;

  if (name == 0 || !parse_pkgname(name, 6))
    return 0;
    
  p = g_new0(struct db_pkg, 1);
  p->name = parse_pkgname(name, 5);
  p->shortname = parse_pkgname(name, 1);
  p->version = parse_pkgname(name, 2);
  p->arch = parse_pkgname(name, 3);
  p->build = parse_pkgname(name, 4);
  return p;
}

struct db_file* db_alloc_file(gchar* path, gchar* link)
{
  struct db_file* f;
  f = g_new0(struct db_file, 1);
  f->path = path;
  f->link = link;
  return f;
}

gint db_add_pkg(struct db_pkg* pkg)
{
  sql_query *q;
  GSList* l;

  _db_reset_error();
  _db_open_check(1)
  
  /* check if pkg contains everthing required */
  if (pkg == 0 || pkg->name == 0 || pkg->shortname == 0 
      || pkg->version == 0 || pkg->build == 0 || pkg->files == 0)
  {
    _db_set_error(DB_OTHER, "can't add package to the database (incomplete package structure)");
    return 1;
  }
  /* sql error handler */
  sql_push_context(SQL_ERRJUMP,1);
  if (setjmp(sql_errjmp) == 1)
  { /* sql exception occured */
    _db_set_error(DB_OTHER, "can't add package to the database (sql error)\n%s", sql_error());
    sql_pop_context(0);
    return 1;
  }

  /* check if package already exists in db */
  q = sql_prep("SELECT id FROM packages WHERE name == '%q';", pkg->name);
  if (sql_step(q))
  { /* if package exists */
    _db_set_error(DB_EXIST, "can't add package to the database (same package is already there - %s)", pkg->name);
    sql_pop_context(0);
    return 1;
  }
  sql_fini(q);

  guint fi_size = g_slist_length(pkg->files);
  guint32 *fi_array = g_malloc(sizeof(guint32)*fi_size);
  guint i = 0;

  for (l=pkg->files; l!=0; l=l->next)
  { /* for each file */
    struct db_file* f = l->data;
    struct fdb_file fdb;
    fdb.path = f->path;
    fdb.link = f->link;    
    fdb.mode = f->mode;    
    f->id = fdb_add_file(&fdb);
    f->refs = fdb.refs;
    fi_array[i++] = f->id;
  }

  /* add pkg to the pacakge table */
  q = sql_prep("INSERT INTO packages(name, shortname, version, arch, build, csize, usize, desc, location, files)"
               " VALUES(?,?,?,?,?,?,?,?,?,?);");
  sql_set_text(q, 1, pkg->name);
  sql_set_text(q, 2, pkg->shortname);
  sql_set_text(q, 3, pkg->version);
  sql_set_text(q, 4, pkg->arch);
  sql_set_text(q, 5, pkg->build);
  sql_set_int(q, 6, pkg->csize);
  sql_set_int(q, 7, pkg->usize);
  sql_set_text(q, 8, pkg->desc);
  sql_set_text(q, 9, pkg->location);
  sql_set_blob(q, 10, fi_array, fi_size*sizeof(*fi_array));
  sql_step(q);
  sql_fini(q);

  g_free(fi_array);

  sql_pop_context(1);
  return 0;
}

gint db_rem_pkg(gchar* name)
{
  sql_query *q;
  gint pid;
  
  _db_reset_error();
  _db_open_check(1)

  if (name == 0)
  {
    _db_set_error(DB_OTHER, "can't remove package from the database (name not given)");
    return 1;
  }
  
  /* sql error handler */
  sql_push_context(SQL_ERRJUMP,1);
  if (setjmp(sql_errjmp) == 1)
  { /* sql exception occured */
    _db_set_error(DB_OTHER, "can't remove package from the database (sql error)\n%s", sql_error());
    sql_pop_context(0);
    return 1;
  }

  /* check if package is in db */
  q = sql_prep("SELECT id,files FROM packages WHERE name == '%q';", name);
  if (!sql_step(q))
  { /* if package does not exists */
    _db_set_error(DB_NOTEX, "can't remove package from the database (package is not there - %s)", name);
    sql_pop_context(0);
    return 1;
  }
  pid = sql_get_int(q, 0);

  guint fi_size = sql_get_size(q, 1)/sizeof(guint32);
  guint32 *fi_array = (guint32*)sql_get_blob(q, 1);
  guint i;
  for (i=0; i<fi_size; i++)
    fdb_del_file(fi_array[i]);
  
  sql_fini(q);

  /* remove package from packages table */
  sql_exec("DELETE FROM packages WHERE id == %d;", pid);

  sql_pop_context(1);
  return 0;
}

struct db_pkg* db_get_pkg(gchar* name, gboolean files)
{
  sql_query *q;
  struct db_pkg* p=0;
  gint pid;

  _db_reset_error();
  _db_open_check(0)

  if (name == 0)
  {
    _db_set_error(DB_OTHER, "can't retrieve package from the database (name not given)");
    return 0;
  }

  /* sql error handler */
  sql_push_context(SQL_ERRJUMP,1);
  if (setjmp(sql_errjmp) == 1)
  { /* sql exception occured */
    _db_set_error(DB_OTHER, "can't retrieve package from the database (sql error)\n%s", sql_error());
    sql_pop_context(0);
    db_free_pkg(p);
    return 0;
  }

  q = sql_prep("SELECT id, name, shortname, version, arch, build, csize,"
                   " usize, desc, location, files FROM packages WHERE name == '%q';", name);
  if (!sql_step(q))
  {
    _db_set_error(DB_NOTEX, "can't retrieve package from the database (package is not there - %s)", name);
    sql_pop_context(0);
    return 0;
  }

  p = g_new0(struct db_pkg, 1);
  pid = sql_get_int(q, 0);
  p->name = g_strdup(name);
  p->shortname = g_strdup(sql_get_text(q, 2));
  p->version = g_strdup(sql_get_text(q, 3));
  p->arch = g_strdup(sql_get_text(q, 4));
  p->build = g_strdup(sql_get_text(q, 5));
  p->csize = sql_get_int(q, 6);
  p->usize = sql_get_int(q, 7);
  p->desc = g_strdup(sql_get_text(q, 8));
  p->location = g_strdup(sql_get_text(q, 9));

  /* caller don't want files list, so it's enough here */
  if (files == 0)
  {
    sql_pop_context(0);
    return p;
  }

  guint fi_size = sql_get_size(q, 10)/sizeof(guint32);
  guint32 *fi_array = (guint32*)sql_get_blob(q, 10);
  
  guint i;
  struct fdb_file f;
  for (i=0; i<fi_size; i++)
  {
    fdb_get_file(fi_array[i], &f);
    struct db_file* file = g_new0(struct db_file, 1);
    file->path = g_strdup(f.path);
    file->link = f.link?g_strdup(f.link):0;
    file->mode = f.mode;
    file->id = fi_array[i];
    p->files = g_slist_append(p->files, file);
  }

  sql_pop_context(0);
  return p;
}

struct db_pkg* db_legacy_get_pkg(gchar* name)
{
  gchar *tmpstr, *linktgt;
  FILE *fp, *fs, *f;
  gchar *ln = 0;
  gsize len = 0, l;
  enum { HEADER, FILELIST, LINKLIST } state = HEADER;
  struct db_pkg* p=0;
  regex_t re_symlink,
          re_pkgname,
          re_pkgsize,
          re_desc,
          re_nameparts;
  regmatch_t rm[5];

  if (name == 0)
    return 0;

  /* open package db entries */  
  tmpstr = g_strjoin("/", _db_topdir, "packages", name, 0);
  fp = fopen(tmpstr, "r");
  g_free(tmpstr);
  tmpstr = g_strjoin("/", _db_topdir, "scripts", name, 0);
  fs = fopen(tmpstr, "r");
  g_free(tmpstr);

  /* if main package db entry is not accessible return error */
  if (fp == NULL)
    goto err;

  /* compile regexps */
  if (regcomp(&re_symlink, "^\\( cd ([^ ]+) ; ln -sf ([^ ]+) ([^ ]+) \\)$", REG_EXTENDED) || 
      regcomp(&re_pkgname, "^([^:]+):[ ]*(.+)?$", REG_EXTENDED) ||
      regcomp(&re_desc, "^([^:]+):(.*)$", REG_EXTENDED) ||
      regcomp(&re_pkgsize, "^([^:]+):[ ]*([0-9]+) K$", REG_EXTENDED) ||
      regcomp(&re_nameparts, "^(.+)-([^-]+)-([^-]+)-([^-]+)$", REG_EXTENDED))
    g_error("can't compile regexps");

  p = g_new0(struct db_pkg, 1);
  p->name = g_strdup(name);
  p->shortname = parse_pkgname(p->name, 1);
  p->version = parse_pkgname(p->name, 2);
  p->arch = parse_pkgname(p->name, 3);
  p->build = parse_pkgname(p->name, 4);
    
  /* for each line in the main package db entry file do: */
  f = fp;
  while (1)
  {
    if (getline(&ln, &len, f) == -1)
    { /* handle EOF */
      if (state == FILELIST)
      {
        if (fs == NULL) /* no linklist */
          break;
        f = fs;
        state = LINKLIST;
        continue;
      }
      else if (state == LINKLIST)
        break;
      goto err;
    }

    /* remove newline character */
    l = strlen(ln);
    if (l > 0 && ln[l-1] == '\n')
      ln[l-1] = '\0';

    switch (state)
    {
      case HEADER:
      {
        if (!regexec(&re_pkgsize, ln, 3, rm, 0))
        {
          ln[rm[1].rm_eo] = 0;
          ln[rm[2].rm_eo] = 0;
          if (!strcmp(ln, "COMPRESSED PACKAGE SIZE"))
          {
            p->csize = atol(ln+rm[2].rm_so);
          }
          else if (!strcmp(ln, "UNCOMPRESSED PACKAGE SIZE"))
          {
            p->usize = atol(ln+rm[2].rm_so);
          }
          else
            goto err;
        }
        else if (!regexec(&re_pkgname, ln, 3, rm, 0))
        {
          ln[rm[1].rm_eo] = 0;
          if (!strcmp(ln, "PACKAGE NAME") && rm[2].rm_so > 0)
          {
            ln[rm[2].rm_eo] = 0;
            if (strcmp(p->name, ln+rm[2].rm_so)) /* pkgname != requested pkgname */
              goto err;
          }
          else if (!strcmp(ln, "PACKAGE LOCATION") && rm[2].rm_so > 0)
          {
            ln[rm[2].rm_eo] = 0;
            p->location = g_strdup(ln+rm[2].rm_so);
          }
          else if (!strcmp(ln, "PACKAGE DESCRIPTION"))
          {
          }
          else if (!strcmp(ln, "FILE LIST"))
          {
            state = FILELIST;
          }
          else if (!strcmp(ln, p->shortname))
          {
            ln[rm[1].rm_eo] = ':';
            if (p->desc)
            {
              tmpstr = g_strconcat(p->desc, ln, "\n", 0);
              g_free(p->desc);
              p->desc = tmpstr;
            }
            else
              p->desc = g_strconcat(ln, "\n", 0);
          }
          else
            goto err;
        }
        else
          goto err;
      }
      break;
      case FILELIST:
        p->files = g_slist_append(p->files, db_alloc_file(g_strdup(ln),0));
      break;
      case LINKLIST:
      {
        if (regexec(&re_symlink, ln, 4, rm, 0))
          continue;
        ln[rm[1].rm_eo] = 0;
        ln[rm[2].rm_eo] = 0;
        ln[rm[3].rm_eo] = 0;
        tmpstr = g_strjoin("/", ln+rm[1].rm_so, ln+rm[3].rm_so, 0);
        linktgt = g_strdup(ln+rm[2].rm_so);
        p->files = g_slist_append(p->files, db_alloc_file(tmpstr, linktgt));
      }
      break;
      default:
        goto err;
    }
  }

  goto err1;
 err:
  db_free_pkg(p);
  p = 0;
 err1:
  regfree(&re_symlink);
  regfree(&re_pkgname);
  regfree(&re_pkgsize);
  regfree(&re_desc);
  regfree(&re_nameparts);
  if (ln) free(ln);
  if (fp) fclose(fp);
  if (fs) fclose(fs);
  return p;
}

gint db_legacy_add_pkg(struct db_pkg* pkg)
{
  GSList* l;
  FILE* pf;
  FILE* sf;
  gchar *ppath, *spath;
  gint ret = 1;

  _db_reset_error();
  _db_open_check(1)

  /* check if pkg contains everthing required */
  if (pkg == 0 || pkg->name == 0 || pkg->files == 0)
  {
    _db_set_error(DB_OTHER, "can't add package to the legacy database (incomplete package structure)");
    return 1;
  }

  ppath = g_strdup_printf("%s/%s.%s", _db_dbroot, "packages", pkg->name);
  spath = g_strdup_printf("%s/%s.%s", _db_dbroot, "scripts", pkg->name);
/*XXX: real code
  ppath = g_strdup_printf("%s/%s/%s", _db_topdir, "packages", pkg->name);
  spath = g_strdup_printf("%s/%s/%s", _db_topdir, "scripts", pkg->name);
*/
  pf = fopen(ppath, "w");
  if (pf == 0)
    goto err_0;
  sf = fopen(spath, "w");
  if (sf == 0)
    goto err_1;

  /* construct header */
  fprintf(pf,
    "PACKAGE NAME:              %s\n"
    "COMPRESSED PACKAGE SIZE:   %d K\n"
    "UNCOMPRESSED PACKAGE SIZE: %d K\n"
    "PACKAGE LOCATION:          %s\n"
    "PACKAGE DESCRIPTION:\n"
    "%s",
    pkg->name, pkg->csize, pkg->usize, pkg->location?pkg->location:"", pkg->desc?pkg->desc:""
  );
  
  /* construct filelist and script for links creation */
  fprintf(pf, "FILE LIST:\n");
  
  for (l=pkg->files; l!=0; l=l->next)
  {
    struct db_file* f = l->data;
    if (f->link)
    {
      gchar* bn = sys_dirname(f->path);
      gchar* dn = sys_basename(f->path);
      fprintf(sf, "( cd %s ; rm -rf %s )\n"
                  "( cd %s ; ln -sf %s %s )\n", dn, bn, dn, f->link, bn);
    }
    else
      fprintf(pf, "%s\n", f->path);
  }

  ret = 0;
  fclose(sf);
 err_1:
  fclose(pf);
 err_0:
  g_free(ppath);
  g_free(spath);
  return ret;
}

void db_free_pkg(struct db_pkg* pkg)
{
  struct db_pkg* p = pkg;
  GSList* l;
  if (p == 0)
    return;
  if (p->files) {
    for (l=p->files; l!=0; l=l->next)
    {
      struct db_file* f = l->data;
      g_free(f->path);
      g_free(f->link);
      g_free(f);
      l->data = 0;
    }
    g_slist_free(p->files);
  }
  g_free(p->name);
  g_free(p->location);
  g_free(p->desc);
  g_free(p->shortname);
  g_free(p->version);
  g_free(p->arch);
  g_free(p->build);
  g_free(p);
}

void db_free_packages(GSList* pkgs)
{
  GSList* l;
  if (pkgs == 0)
    return;
  for (l=pkgs; l!=0; l=l->next)
    db_free_pkg(l->data);
  g_slist_free(pkgs);
}

GSList* db_get_packages()
{
  sql_query *q;
  GSList *pkgs=0;

  _db_reset_error();
  _db_open_check(0)
  
  /* sql error handler */
  sql_push_context(SQL_ERRJUMP,1);
  if (setjmp(sql_errjmp) == 1)
  { /* sql exception occured */
    _db_set_error(DB_OTHER, "can't get packages from the database (sql error)\n%s", sql_error());
    sql_pop_context(0);
    db_free_packages(pkgs);
    return 0;
  }

  q = sql_prep("SELECT id, name, shortname, version, arch, build, csize,"
                   " usize, desc, location FROM packages ORDER BY name;");
  while(sql_step(q))
  {
    struct db_pkg* p;
    p = g_new0(struct db_pkg, 1);
    p->id = sql_get_int(q, 0);
    p->name = g_strdup(sql_get_text(q, 1));
    p->shortname = g_strdup(sql_get_text(q, 2));
    p->version = g_strdup(sql_get_text(q, 3));
    p->arch = g_strdup(sql_get_text(q, 4));
    p->build = g_strdup(sql_get_text(q, 5));
    p->csize = sql_get_int(q, 6);
    p->usize = sql_get_int(q, 7);
    p->desc = g_strdup(sql_get_text(q, 8));
    p->location = g_strdup(sql_get_text(q, 9));

    pkgs = g_slist_append(pkgs, p);
  }

  sql_pop_context(0);
  return pkgs;
}

gint db_sync_to_legacydb()
{
  GSList *pkgs, *l;
  gint ret = 1;

  _db_reset_error();
  _db_open_check(1)

  pkgs = db_get_packages();
  if (pkgs == 0 && db_error() == 0)
    return 0; /* no packages */
  else if (pkgs == 0)
  {
    gchar* err = g_strdup(db_error());
    _db_set_error(DB_OTHER, "can't synchronize database (internal error)\n%s", err);
    g_free(err);
    goto err_0;
  }
 
  for (l=pkgs; l!=0; l=l->next)
  { /* for each package */
    struct db_pkg* pkg = l->data;
    struct db_pkg* p;

/*XXX: debug code */
//    printf("syncing %s\n", pkg->name);
//    fflush(stdout);
/*XXX: debug code */

    p = db_get_pkg(pkg->name,1);
    if (p == 0)
    {
      gchar* err = g_strdup(db_error());
      _db_set_error(DB_OTHER, "can't synchronize database (internal error)\n%s", err);
      g_free(err);
      goto err_1;
    }
    if (db_legacy_add_pkg(p))
    {
      gchar* err = g_strdup(db_error());
      _db_set_error(DB_OTHER, "can't synchronize database (internal error)\n%s", err);
      g_free(err);
      goto err_1;
    }
    db_free_pkg(p);
  }

  ret = 0;
 err_1:
  db_free_packages(pkgs);
 err_0:
  return ret;
}

gint db_sync_from_legacydb()
{
  DIR* d;
  struct dirent* de;
  gchar* tmpstr = g_strdup_printf("%s/%s", _db_topdir, "packages");
  gint ret = 1;

  _db_reset_error();
  _db_open_check(1)

  d = opendir(tmpstr);
  g_free(tmpstr);
  if (d == NULL)
  {
    _db_set_error(DB_OTHER, "can't synchronize database (legacy database directory not found)");
    goto err_0;
  }
  
  /*XXX: unchecked sql library call */
  sql_exec("DELETE FROM packages;");

  while ((de = readdir(d)) != NULL)
  {
    struct db_pkg* p=0;

    if (!strcmp(de->d_name,".") || !strcmp(de->d_name,".."))
      continue;

/*XXX: debug code */
//    printf("syncing %s\n", de->d_name);
//    fflush(stdout);
/*XXX: debug code */

    p = db_legacy_get_pkg(de->d_name);
    if (p == 0)
    {
      gchar* err = g_strdup(db_error());
      _db_set_error(DB_OTHER, "can't synchronize database (internal error)\n%s", err);
      g_free(err);
      goto err_1;
    }
    if (db_add_pkg(p))
    {
      gchar* err = g_strdup(db_error());
      _db_set_error(DB_OTHER, "can't synchronize database (internal error)\n%s", err);
      g_free(err);
      db_free_pkg(p);
      goto err_1;
    }
    db_free_pkg(p);
  }

  ret = 0;
 err_1:
  closedir(d);
 err_0:
  return ret;
}