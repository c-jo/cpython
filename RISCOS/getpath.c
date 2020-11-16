/* Return the initial module search path - RISC OS Version. */

#include "Python.h"
#include "pycore_initconfig.h"
#include "osdefs.h"
#include "pycore_fileutils.h"
#include "pycore_pathconfig.h"
#include "pycore_pystate.h"

#include "swis.h"

#include <sys/types.h>
#include <string.h>

/* Search in some common locations for the associated Python libraries.
 * This is a variant of the posix system.
 *
 * Two directories must be found, the platform independent directory
 * (prefix), containing the common .py and .pyc files, and the platform
 * dependent directory (exec_prefix), containing the shared library
 * modules.  Note that prefix and exec_prefix can be the same directory,
 * but for some installations, they are different.
 *
 * Py_GetPath() carries out separate searches for prefix and exec_prefix.
 * Each search tries a number of different locations until a ``landmark''
 * file or directory is found.  If no prefix or exec_prefix is found, a
 * warning message is issued and the preprocessor defined PREFIX and
 * EXEC_PREFIX are used (even though they will not work); python carries on
 * as best as is possible, but most imports will fail.
 *
 * Before any searches are done, the location of the executable is
 * determined. On RISC OS argv[0] is always the full pathname so
 * argv0_path is set to the directory containing the executable
 * (i.e. the last component is stripped).
 *
 * With argv0_path in hand, we perform a number of steps.  The same steps
 * are performed for prefix and for exec_prefix, but with a different
 * landmark.
 *
 * Step 1. Are we running python out of the build directory?  This is
 * checked by looking for a different kind of landmark relative to
 * argv0_path.  For prefix, the landmark's path is derived from the VPATH
 * preprocessor variable (taking into account that its value is almost, but
 * not quite, what we need).  For exec_prefix, the landmark is
 * pybuilddir/txt.  If the landmark is found, we're done.
 *
 * For the remaining steps, the prefix landmark will always be
 * lib.pythonXY.os(/py) and the exec_prefix will always be
 * lib.pythonXY.lib-dynload, where XY is the version number without dots.
 * Note that this means that no more build directory checking is performed;
 * if the first step did not find the landmarks, the assumption is that python
 * is running from an installed setup.
 *
 * Step 2. See if the $PYTHONHOME environment variable points to the
 * installed location of the Python libraries.  If $PYTHONHOME is set, then
 * it points to prefix and exec_prefix.  $PYTHONHOME can be a single
 * directory, which is used for both, or the prefix and exec_prefix
 * directories separated by a comma.
 *
 * Step 3. Try to find prefix and exec_prefix relative to argv0_path,
 * backtracking up the path until it is exhausted.  This is the most common
 * step to succeed.  Note that if prefix and exec_prefix are different,
 * exec_prefix is more likely to be found; however if exec_prefix is a
 * subdirectory of prefix, both will be found.
 *
 * Step 3. Search the directories pointed to by the preprocessor variables
 * PREFIX and EXEC_PREFIX.  These are supplied by the Makefile but can be
 * passed in as options to the configure script. They are both set to
 * <Python3$Dir>. The variables will be canonicalised, so the current run-time
 * values of those variables will be used.
 *
 * That's it!
 *
 * Well, almost.  Once we have determined prefix and exec_prefix, the
 * preprocessor variable PYTHONPATH is used to construct a path.  Each
 * relative path on PYTHONPATH is prefixed with prefix.  Then the directory
 * containing the shared library modules is appended.  The environment
 * variable $PYTHONPATH is inserted in front of it all.  Finally, the
 * prefix and exec_prefix globals are tweaked so they reflect the values
 * expected by other code, by stripping the "lib/python$VERSION/..." stuff
 * off.  If either points to the build directory, the globals are reset to
 * the corresponding preprocessor variables (so sys.prefix will reflect the
 * installation location, even though sys.path points into the build
 * directory).  This seems to make more sense given that currently the only
 * known use of sys.prefix and sys.exec_prefix is for the ILU installation
 * process to find the installed Python tree.
 *
 * An embedding application can use Py_SetPath() to override all of
 * these authomatic path computations.
 *
 * NOTE: Windows MSVC builds use PC/getpathp.c instead.
 */

#ifdef __cplusplus
extern "C" {
#endif

#if !defined(PREFIX) || !defined(EXEC_PREFIX) || !defined(VERSION) || !defined(VPATH)
#error "PREFIX, EXEC_PREFIX, VERSION, and VPATH must be constant defined"
#endif

#ifndef LANDMARK
#define LANDMARK "os"
#endif

#define DECODE_LOCALE_ERR(NAME, LEN) \
    ((LEN) == (size_t)-2) \
     ? _PyStatus_ERR("cannot decode " NAME) \
     : _PyStatus_NO_MEMORY()

#define PATHLEN_ERR() _PyStatus_ERR("path configuration: path too long")

typedef struct {
    char *python_home;        /* PYTHONHOME */
    char *pythonpath;         /* PYTHONPATH macro */
    char *prefix;             /* PREFIX macro (canonicalised) */
    char *exec_prefix;        /* EXEC_PREFIX macro (canonicalised) */

    char *lib_python;         /* "pythonXY.lib" */

    int prefix_found;         /* found platform independent libraries? */
    int exec_prefix_found;    /* found the platform dependent libraries? */

    int warnings;
    char *pythonpath_env;
} PyCalculatePath;

static const char delimiter[2] = {DELIM, '\0'};
static const char separator[2] = {SEP, '\0'};

static void
reduce(char *dir)
{
    size_t i = strlen(dir);
    while (i > 0 && dir[i] != SEP) {
        --i;
    }
    dir[i] = '\0';
}

/* canonicalise (FS_Control 37) and return a PyMem_Malloc-ed string */
static char *
Py_canonicialise(const char *path)
{
   int size = 0;
   if (_swix(OS_FSControl, _INR(0,5) | _OUT(5),
                 37, path, 0, 0, 0, 0, &size ))
   {
       return _PyMem_RawStrdup(path);
   }
   size = 1-size;
   char *res = PyMem_Malloc(size);
   if (_swix(OS_FSControl, _INR(0,5) | _OUT(5),
                 37, path, res, 0, 0, size, &size ))
   {
       PyMem_Free(res);
       return _PyMem_RawStrdup(path);
   }
   return res;
}

/* get RISC OS objet type (file, directory or image) */
static int
get_obj_type(const char *pathname)
{
    int obj_type = 0;
    if (_swix(OS_File, _INR(0,1) | _OUT(0),
                       17, pathname,
                       &obj_type))
    {
        return -1;
    }
    return obj_type;
}

/* get RISC OS filetype */
static int
get_filetype(const char *filename)
{
    int filetype;
    if (!_swix(OS_File, _INR(0,1) | _OUT(6),
                        23, filename,
                        &filetype))
    {
        return filetype;
    }
    return -1;
}

/* Is file, not directory */
static int
isfile(const char *filename)
{
    return get_obj_type(filename) == 1;  // File
}

/* Is directory */
static int
isdir(char *filename)
{
    int obj_type = get_obj_type(filename);
    return (obj_type == 2 || obj_type == 3); // Directory or image
}

/* Is 'module' in 'directory'? Check types and for /pyc too. */
static int
ismodule(const char *directory, const char *module)
{
    char buffer[256];
    strcpy(buffer, directory);
    strcat(buffer, ".");
    strcat(buffer, module);

    // Is ita typed file?
    int filetype = get_filetype(buffer);
    if (filetype == 0xa73 || filetype == 0xa74)
         return 1; // It's a python one

    // add /py and try that?
    strcat(buffer, "/py");
    if (isfile(buffer))
            return 1;

    // try the /pyc?
    strcat(buffer, "c");
    if (isfile(buffer))
            return 1;

    return 0;
}

/* Add a path component, by appending stuff to buffer.
   buflen: 'buffer' length in characters including trailing NUL. */
static PyStatus
joinpath(char *buffer, const char *stuff, size_t buflen)
{
    size_t n, k;
    if (stuff[0] != SEP) {
        n = strlen(buffer);
        if (n >= buflen) {
            return PATHLEN_ERR();
        }

        if (n > 0 && buffer[n-1] != SEP) {
            buffer[n++] = SEP;
        }
    }
    else {
        n = 0;
    }

    k = strlen(stuff);
    if (n + k >= buflen) {
        return PATHLEN_ERR();
    }
    strncpy(buffer+n, stuff, k);
    buffer[n+k] = '\0';

    return _PyStatus_OK();
}

static inline int
safe_strcpy(char *dst, const char *src, size_t n)
{
    size_t srclen = strlen(src);
    if (n <= srclen) {
        dst[0] = L'\0';
        return -1;
    }
    memcpy(dst, src, (srclen + 1) * sizeof(char));
    return 0;
}

/* search_for_prefix requires that argv0_path be no more than MAXPATHLEN
   bytes long.
*/
static PyStatus
search_for_prefix(PyCalculatePath *calculate, _PyPathConfig *pathconfig,
                  const char *argv0_path,
                  char *prefix, size_t prefix_len, int *found)
{
    char path[MAXPATHLEN+1];
    memset(path, 0, sizeof(path));
    size_t path_len = Py_ARRAY_LENGTH(path);

    PyStatus status;

    /* If PYTHONHOME is set, we believe it unconditionally */
    if (pathconfig->home) {
        printf("got PYTHONHOME\n");
        /* Path: <home> / <lib_python> */
        if (safe_strcpy(prefix, Py_EncodeLocale(pathconfig->home, NULL), prefix_len) < 0) {
            return PATHLEN_ERR();
        }
        char *delim = strchr(prefix, DELIM);
        if (delim) {
            *delim = L'\0';
        }
        status = joinpath(prefix, calculate->lib_python, prefix_len);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
        *found = 1;
        return _PyStatus_OK();
    }

    /* Check to see if argv[0] is in the build directory */
    if (safe_strcpy(path, argv0_path, path_len) < 0) {
        return PATHLEN_ERR();
    }

    status = joinpath(path, "Modules.Setup/local", path_len);
    if (_PyStatus_EXCEPTION(status)) {
         return status;
    }

    if (isfile(path)) {
        /* Check VPATH to see if argv0_path is in the build directory.
           VPATH can be empty. */
        if (VPATH != NULL) {
            /* Path: <argv0_path> / <vpath> / Lib / LANDMARK */
            if (safe_strcpy(prefix, argv0_path, prefix_len) < 0) {
                return PATHLEN_ERR();
            }

            if (strlen(VPATH) > 0)
            {
                status = joinpath(prefix, VPATH, prefix_len);
                PyMem_RawFree(VPATH);
                if (_PyStatus_EXCEPTION(status)) {
                    return status;
                }
            }

            status = joinpath(prefix, "lib", prefix_len);
            if (_PyStatus_EXCEPTION(status)) {
                return status;
            }

            if (ismodule(prefix, LANDMARK)) {
                *found = -1;
                return _PyStatus_OK();
            }
        }
    }

    /* Search from argv0_path, until root is found */
    if (safe_strcpy(prefix, argv0_path, prefix_len) < 0) {
        return PATHLEN_ERR();
    }

    do {
        /* Path: <argv0_path or substring> / <lib_python> / LANDMARK */
        size_t n = strlen(prefix);
        status = joinpath(prefix, calculate->lib_python, prefix_len);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }

        if (ismodule(prefix, LANDMARK)) {
            *found = 1;
            //reduce(prefix);
            return _PyStatus_OK();
        }
        prefix[n] = '\0';

        size_t ln = strlen(prefix);

        if (ln > 1)
        {
            char lc = prefix[ln-1];
            if (lc == '$' || lc == '&' || lc == '%')
                prefix[0] = 0;
        }
        if (prefix[0])
            reduce(prefix);
    } while (prefix[0]);

    /* Look at configure's PREFIX.
       Path: <PREFIX macro> / <lib_python> / LANDMARK */
    if (safe_strcpy(prefix, calculate->prefix, prefix_len) < 0) {
        return PATHLEN_ERR();
    }
    status = joinpath(prefix, calculate->lib_python, prefix_len);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    if (ismodule(prefix, LANDMARK)) {
        *found = 1;
        return _PyStatus_OK();
    }

    /* Fail */
    *found = 0;
    return _PyStatus_OK();
}


static PyStatus
calculate_prefix(PyCalculatePath *calculate, _PyPathConfig *pathconfig,
                 const char *argv0_path,
                 char *prefix, size_t prefix_len)
{
    PyStatus status;

    status = search_for_prefix(calculate, pathconfig, argv0_path,
                               prefix, prefix_len,
                               &calculate->prefix_found);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    if (!calculate->prefix_found) {
        if (calculate->warnings) {
            fprintf(stderr,
                "Could not find platform independent libraries <prefix>\n");
        }
        if (safe_strcpy(prefix, calculate->prefix, prefix_len) < 0) {
            return PATHLEN_ERR();
        }
        status = joinpath(prefix, calculate->lib_python, prefix_len);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
    }
    return _PyStatus_OK();
}


static PyStatus
calculate_set_prefix(PyCalculatePath *calculate, _PyPathConfig *pathconfig,
                     char *prefix)
{
    /* Reduce prefix and exec_prefix to their essence,
     * e.g. /usr/local/lib/python1.5 is reduced to /usr/local.
     * If we're loading relative to the build directory,
     * return the compiled-in defaults instead.
     */
    size_t len = 0;
    if (calculate->prefix_found > 0) {
        reduce(prefix);
        reduce(prefix);
        /* The prefix is the root directory, but reduce() chopped
         * off the "/". */
        //if (!prefix[0]) {
        //    strcpy(prefix, separator);
        //}
        pathconfig->prefix = Py_DecodeLocale(prefix, &len);
        if (!pathconfig->prefix) {
            return DECODE_LOCALE_ERR("Calculated Prefix", len);
        }
    }
    else
    {
        pathconfig->prefix = Py_DecodeLocale(calculate->prefix, &len);
        if (!pathconfig->prefix) {
            return DECODE_LOCALE_ERR("Calculated Prefix", len);
        }
    }
    return _PyStatus_OK();
}


static PyStatus
calculate_pybuilddir(const char *argv0_path,
                     char *exec_prefix, size_t exec_prefix_len,
                     int *found)
{
    PyStatus status;

    char filename[MAXPATHLEN+1];
    memset(filename, 0, sizeof(filename));
    size_t filename_len = Py_ARRAY_LENGTH(filename);

    /* Check to see if argv[0] is in the build directory. "pybuilddir.txt"
       is written by setup.py and contains the relative path to the location
       of shared library modules.

       Filename: <argv0_path> / "pybuilddir.txt" */
    if (safe_strcpy(filename, argv0_path, filename_len) < 0) {
        return PATHLEN_ERR();
    }

    status = joinpath(filename, "pybuilddir/txt", filename_len);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    if (!isfile(filename)) {
        return _PyStatus_OK();
    }

    FILE *fp = fopen(filename, "rb");
    if (fp == NULL) {
        errno = 0;
        return _PyStatus_OK();
    }

    char pybuilddir[MAXPATHLEN + 1];
    size_t n = fread(pybuilddir, 1, Py_ARRAY_LENGTH(pybuilddir) - 1, fp);
    pybuilddir[n] = '\0';
    fclose(fp);

    /* Path: <argv0_path> / <pybuilddir content> */
    if (safe_strcpy(exec_prefix, argv0_path, exec_prefix_len) < 0) {
        return PATHLEN_ERR();
    }
    status = joinpath(exec_prefix, pybuilddir, exec_prefix_len);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    *found = -1;
    return _PyStatus_OK();
}


/* search_for_exec_prefix requires that argv0_path be no more than
   MAXPATHLEN bytes long.
*/
static PyStatus
search_for_exec_prefix(PyCalculatePath *calculate, _PyPathConfig *pathconfig,
                       const char *argv0_path,
                       char *exec_prefix, size_t exec_prefix_len,
                       int *found)
{
    PyStatus status;

    /* If PYTHONHOME is set, we believe it unconditionally */
    if (calculate->python_home) {
        /* Path: <home> / <lib_python> / "lib-dynload" */
        char *delim = strchr(calculate->python_home, DELIM);
        if (delim) {
            if (safe_strcpy(exec_prefix, delim+1, exec_prefix_len) < 0) {
                return PATHLEN_ERR();
            }
        }
        else {
            if (safe_strcpy(exec_prefix, calculate->python_home, exec_prefix_len) < 0) {
                return PATHLEN_ERR();
            }
        }
        status = joinpath(exec_prefix, calculate->lib_python, exec_prefix_len);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
        status = joinpath(exec_prefix, "lib-dynload", exec_prefix_len);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
        *found = 1;
        return _PyStatus_OK();
    }

    /* Check for pybuilddir.txt */
    assert(*found == 0);
    status = calculate_pybuilddir(argv0_path, exec_prefix, exec_prefix_len,
                                  found);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }
    if (*found) {
        return _PyStatus_OK();
    }

    /* Search from argv0_path, until root is found */
    if (safe_strcpy(exec_prefix, argv0_path, exec_prefix_len) < 0) {
        return PATHLEN_ERR();
    }

    do {
        /* Path: <argv0_path or substring> / <lib_python> / "lib-dynload" */
        size_t n = strlen(exec_prefix);
        status = joinpath(exec_prefix, calculate->lib_python, exec_prefix_len);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
        status = joinpath(exec_prefix, "lib-dynload", exec_prefix_len);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
        if (isdir(exec_prefix)) {
            *found = 1;
            return _PyStatus_OK();
        }
        exec_prefix[n] = L'\0';
        reduce(exec_prefix);
    } while (exec_prefix[0]);

    /* Look at configure's EXEC_PREFIX.

       Path: <EXEC_PREFIX macro> / <lib_python> / "lib-dynload" */
    if (safe_strcpy(exec_prefix, calculate->exec_prefix, exec_prefix_len) < 0) {
        return PATHLEN_ERR();
    }
    status = joinpath(exec_prefix, calculate->lib_python, exec_prefix_len);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }
    status = joinpath(exec_prefix, "lib-dynload", exec_prefix_len);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }
    if (isdir(exec_prefix)) {
        *found = 1;
        return _PyStatus_OK();
    }

    /* Fail */
    *found = 0;
    return _PyStatus_OK();
}


static PyStatus
calculate_exec_prefix(PyCalculatePath *calculate, _PyPathConfig *pathconfig,
                      const char *argv0_path,
                      char *exec_prefix, size_t exec_prefix_len)
{
    PyStatus status;

    status = search_for_exec_prefix(calculate, pathconfig, argv0_path,
                                    exec_prefix, exec_prefix_len,
                                    &calculate->exec_prefix_found);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    if (!calculate->exec_prefix_found) {
        if (calculate->warnings) {
            fprintf(stderr,
                "Could not find platform dependent libraries <exec_prefix>\n");
        }
        if (safe_strcpy(exec_prefix, calculate->exec_prefix, exec_prefix_len) < 0) {
            return PATHLEN_ERR();
        }
        status = joinpath(exec_prefix, "lib/lib-dynload", exec_prefix_len);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
    }
    /* If we found EXEC_PREFIX do *not* reduce it!  (Yet.) */
    return _PyStatus_OK();
}


static PyStatus
calculate_set_exec_prefix(PyCalculatePath *calculate,
                          _PyPathConfig *pathconfig,
                          char *exec_prefix)
{
    size_t len = 0;
    if (calculate->exec_prefix_found > 0) {
        reduce(exec_prefix);
        reduce(exec_prefix);
        reduce(exec_prefix);
        //if (!exec_prefix[0]) {
        //    strcpy(exec_prefix, separator);
        //}

        pathconfig->exec_prefix = Py_DecodeLocale(exec_prefix, &len);
        if (!pathconfig->exec_prefix) {
            return DECODE_LOCALE_ERR("Calculated Exec Prefix", len);
        }
    }
    else {
        pathconfig->exec_prefix = Py_DecodeLocale(calculate->exec_prefix, &len);
        if (!pathconfig->exec_prefix) {
            return DECODE_LOCALE_ERR("Calculated Exec Prefix", len);
        }
    }

    if (pathconfig->exec_prefix == NULL) {
        return _PyStatus_NO_MEMORY();
    }

    return _PyStatus_OK();
}


static PyStatus
calculate_argv0_path(PyCalculatePath *calculate, const wchar_t *program_full_path,
                     char *argv0_path, size_t argv0_path_len)
{
    char *_program_full_path = Py_EncodeLocale(program_full_path, NULL);
    if (safe_strcpy(argv0_path, _program_full_path, argv0_path_len) < 0) {
         PyMem_Free(_program_full_path);
        return PATHLEN_ERR();
    }
    PyMem_Free(_program_full_path);
    reduce(argv0_path);
    /* At this point, argv0_path is guaranteed to be less than
       MAXPATHLEN bytes long. */
    return _PyStatus_OK();
}


/* Search for an "pyvenv.cfg" environment configuration file, first in the
   executable's directory and then in the parent directory.
   If found, open it for use when searching for prefixes.
*/
static PyStatus
calculate_read_pyenv(PyCalculatePath *calculate,
                     char *argv0_path, size_t argv0_path_len)
{
    PyStatus status;
#ifdef RISCOS
    const char *env_cfg = "pyvenv/cfg";
#else
    const char *env_cfg = "pyvenv.cfg";
#endif
    FILE *env_file;

    char filename[MAXPATHLEN+1];
    const size_t filename_len = Py_ARRAY_LENGTH(filename);
    memset(filename, 0, sizeof(filename));

    /* Filename: <argv0_path_len> / "pyvenv.cfg" */
    if (safe_strcpy(filename, argv0_path, filename_len) < 0) {
        return PATHLEN_ERR();
    }

    status = joinpath(filename, env_cfg, filename_len);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }
    env_file = fopen(filename, "r");
    if (env_file == NULL) {
        errno = 0;

        /* Filename: <basename(basename(argv0_path_len))> / "pyvenv.cfg" */
        reduce(filename);
        reduce(filename);
        status = joinpath(filename, env_cfg, filename_len);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }

        env_file = fopen(filename, "r");
        if (env_file == NULL) {
            errno = 0;
            return _PyStatus_OK();
        }
    }


    /* Look for a 'home' variable and set argv0_path to it, if found */
    /*
    char home[MAXPATHLEN+1];
    memset(home, 0, sizeof(home));

    if (_Py_FindEnvConfigValue(env_file, "home",
                               home, Py_ARRAY_LENGTH(home))) {
        if (safe_strcpy(argv0_path, home, argv0_path_len) < 0) {
            fclose(env_file);
            return PATHLEN_ERR();
        }
    }
    */
    fclose(env_file);
    return _PyStatus_OK();
}


static PyStatus
calculate_zip_path(PyCalculatePath *calculate, const char *prefix,
                   char *zip_path, size_t zip_path_len)
{
    PyStatus status;

    if (calculate->prefix_found > 0) {
        /* Use the reduced prefix returned by Py_GetPrefix() */
        if (safe_strcpy(zip_path, prefix, zip_path_len) < 0) {
            return PATHLEN_ERR();
        }
        reduce(zip_path);
        reduce(zip_path);
    }
    else {
        if (safe_strcpy(zip_path, calculate->prefix, zip_path_len) < 0) {
            return PATHLEN_ERR();
        }
    }
    status = joinpath(zip_path, "lib.python00/zip", zip_path_len);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    /* Replace "00" with version */
    size_t bufsz = strlen(zip_path);
    zip_path[bufsz - 6] = VERSION[0];
    zip_path[bufsz - 5] = VERSION[2];
    return _PyStatus_OK();
}


static PyStatus
calculate_module_search_path(PyCalculatePath *calculate,
                             _PyPathConfig *pathconfig,
                             const char *prefix,
                             const char *exec_prefix,
                             const char *zip_path)
{
    /* Calculate size of return buffer */
    size_t bufsz = 0;
    if (calculate->pythonpath_env != NULL) {
        bufsz += strlen(calculate->pythonpath_env) + 1;
    }

    char *defpath = calculate->pythonpath;
    size_t prefixsz = strlen(prefix) + 1;
    while (1) {
        char *delim = strchr(defpath, DELIM);

        if (defpath[0] != SEP) {
            /* Paths are relative to prefix */
            bufsz += prefixsz;
        }

        if (delim) {
            bufsz += delim - defpath + 1;
        }
        else {
            bufsz += strlen(defpath) + 1;
            break;
        }
        defpath = delim + 1;
    }

    if (zip_path)
        bufsz += strlen(zip_path) + 1;
    bufsz += strlen(exec_prefix) + 1;

    /* Allocate the buffer */
    char *buf = PyMem_RawMalloc(bufsz * sizeof(char));
    if (buf == NULL) {
        return _PyStatus_NO_MEMORY();
    }
    buf[0] = '\0';

    /* Run-time value of $PYTHONPATH goes first */
    if (calculate->pythonpath_env) {
        strcpy(buf, calculate->pythonpath_env);
        strcat(buf, delimiter);
    }

    /* Next is the default zip path */
    if (zip_path)
    {
        strcat(buf, zip_path);
        strcat(buf, delimiter);
    }

    /* Next goes merge of compile-time $PYTHONPATH with
     * dynamically located prefix.
     */
    defpath = calculate->pythonpath;
    while (1) {
        char *delim = strchr(defpath, DELIM);

        if (defpath[0] != SEP) {
            strcat(buf, prefix);
            if (prefixsz >= 2 && prefix[prefixsz - 2] != SEP &&
                defpath[0] != (delim ? DELIM : L'\0'))
            {
                /* not empty */
                strcat(buf, separator);
            }
        }

        if (delim) {
            size_t len = delim - defpath + 1;
            size_t end = strlen(buf) + len;
            strncat(buf, defpath, len);
            buf[end] = '\0';
        }
        else {
            strcat(buf, defpath);
            break;
        }
        defpath = delim + 1;
    }
    strcat(buf, delimiter);

    /* Finally, on goes the directory for dynamic-load modules */
    strcat(buf, exec_prefix);

    size_t len;
    pathconfig->module_search_path = Py_DecodeLocale(buf, &len);
    if (!pathconfig->module_search_path) {
        return DECODE_LOCALE_ERR("Calculated Prefix", len);
    }

    return _PyStatus_OK();
}


static PyStatus
calculate_init(PyCalculatePath *calculate, const PyConfig *config)
{
    size_t len;

    if (config->home)
    {
        calculate->python_home = Py_EncodeLocale(config->home, &len);
//        if (!calculate->module_search_path) {
//            return DECODE_LOCALE_ERR("Calculated Prefix", len);
//        }
    }
     else
        calculate->python_home = 0;

    calculate->pythonpath = _PyMem_RawStrdup(PYTHONPATH);
    if (!calculate->pythonpath) {
        return _PyStatus_NO_MEMORY();
    }
    calculate->prefix = Py_canonicialise(PREFIX);
    if (!calculate->prefix) {
        return _PyStatus_NO_MEMORY();
    }
    calculate->exec_prefix = Py_canonicialise(EXEC_PREFIX);
    if (!calculate->exec_prefix) {
        return _PyStatus_NO_MEMORY();
    }
    calculate->lib_python = PyMem_Malloc(16);
    if (!calculate->lib_python) {
        return _PyStatus_NO_MEMORY();
    }
    sprintf(calculate->lib_python, "python%c%c.lib", VERSION[0], VERSION[2]);

    calculate->warnings = config->pathconfig_warnings;

    if (config->pythonpath_env)
    {
        calculate->pythonpath_env = Py_EncodeLocale(config->pythonpath_env, &len);
    }
    else
        calculate->pythonpath_env = 0;

    return _PyStatus_OK();
}


static void
calculate_free(PyCalculatePath *calculate)
{
    PyMem_RawFree(calculate->python_home);
    PyMem_RawFree(calculate->pythonpath);
    PyMem_RawFree(calculate->prefix);
    PyMem_RawFree(calculate->exec_prefix);
    PyMem_RawFree(calculate->lib_python);
}


static PyStatus
calculate_path(PyCalculatePath *calculate, _PyPathConfig *pathconfig)
{
    PyStatus status;

    // the program name is the full path
    pathconfig->program_full_path = _PyMem_RawWcsdup(pathconfig->program_name);

    char argv0_path[MAXPATHLEN+1];
    memset(argv0_path, 0, sizeof(argv0_path));

    status = calculate_argv0_path(calculate, pathconfig->program_full_path,
                                  argv0_path, Py_ARRAY_LENGTH(argv0_path));
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    /* If a pyvenv.cfg configure file is found,
       argv0_path is overriden with its 'home' variable. */
    status = calculate_read_pyenv(calculate,
                                  argv0_path, Py_ARRAY_LENGTH(argv0_path));
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    char prefix[MAXPATHLEN+1];
    memset(prefix, 0, sizeof(prefix));
    status = calculate_prefix(calculate, pathconfig,
                              argv0_path,
                              prefix, Py_ARRAY_LENGTH(prefix));
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    char zip_path[MAXPATHLEN+1];    /* "....lib.pythonXY/zip" */
    memset(zip_path, 0, sizeof(zip_path));

    status = calculate_zip_path(calculate, prefix,
                                zip_path, Py_ARRAY_LENGTH(zip_path));
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    char exec_prefix[MAXPATHLEN+1];
    memset(exec_prefix, 0, sizeof(exec_prefix));
    status = calculate_exec_prefix(calculate, pathconfig, argv0_path,
                                   exec_prefix, Py_ARRAY_LENGTH(exec_prefix));
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    if ((!calculate->prefix_found || !calculate->exec_prefix_found) &&
        calculate->warnings)
    {
        fprintf(stderr,
                "Consider setting $PYTHONHOME to <prefix>[,<exec_prefix>]\n");
    }

    if (pathconfig->module_search_path == NULL) {
        char *zip_pathp = 0;
        if (isfile(zip_path))
            zip_pathp = zip_path;
        else
        {
            zip_path[strlen(zip_path)-4] = 0;
            int filetype = get_filetype(zip_path);
            if (filetype == 0xa91)
                zip_pathp = zip_path;
        }
        status = calculate_module_search_path(calculate, pathconfig,
                                              prefix, exec_prefix, zip_pathp);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
    }

    if (pathconfig->prefix == NULL) {
        status = calculate_set_prefix(calculate, pathconfig, prefix);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
    }

    if (pathconfig->exec_prefix == NULL) {
        status = calculate_set_exec_prefix(calculate, pathconfig, exec_prefix);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
    }

    return _PyStatus_OK();
}


/* Calculate the Python path configuration.

   Inputs:

   - PATH environment variable
   - Macros: PYTHONPATH, PREFIX, EXEC_PREFIX, VERSION (ex: "3.9").
     PREFIX and EXEC_PREFIX are generated by the configure script.
     PYTHONPATH macro is the default search path.
   - pybuilddir.txt file
   - pyvenv.cfg configuration file
   - PyConfig fields ('config' function argument):

     - pathconfig_warnings
     - pythonpath_env (PYTHONPATH environment variable)

   - _PyPathConfig fields ('pathconfig' function argument):

     - program_name: see config_init_program_name()
     - home: Py_SetPythonHome() or PYTHONHOME environment variable

   - current working directory: see copy_absolute()

   Outputs, 'pathconfig' fields:

   - program_full_path
   - module_search_path
   - prefix
   - exec_prefix

   If a field is already set (non NULL), it is left unchanged. */
PyStatus
_PyPathConfig_Calculate(_PyPathConfig *pathconfig, const PyConfig *config)
{
    PyStatus status;
    PyCalculatePath calculate;
    memset(&calculate, 0, sizeof(calculate));

    status = calculate_init(&calculate, config);
    if (_PyStatus_EXCEPTION(status)) {
        goto done;
    }

    status = calculate_path(&calculate, pathconfig);
    if (_PyStatus_EXCEPTION(status)) {
        goto done;
    }

    status = _PyStatus_OK();

done:
    calculate_free(&calculate);
    return status;
}

#ifdef __cplusplus
}
#endif
