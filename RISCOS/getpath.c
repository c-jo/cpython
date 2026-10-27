/* Calculate the module search path - RISC OS version.

   This replaces Modules/getpath.c (and the frozen Modules/getpath.py) on
   RISC OS.  It implements _PyConfig_InitPathConfig(), filling in the path
   related members of PyConfig.

   On RISC OS argv[0] is the full pathname of the executable, and '.' is the
   directory separator.  Python is installed as:

       <dir>.Bin.python314              the executable
       <dir>.Python314.Lib              the standard library (prefix)
       <dir>.Python314.Lib.lib-dynload  shared library modules (exec_prefix)
       <dir>.Python314/zip              optional zipped standard library

   where <dir> is normally !Python3.  Since RISC OS filenames are not case
   sensitive, the directories are referred to in lower case below.

   With argv0_path being the directory containing the executable, the
   standard library (prefix) is located as follows:

   1. If PYTHONHOME is set, it is used unconditionally.  It may be a single
      directory, or the prefix and exec_prefix separated by a ','.

   2. If <argv0_path>.<VPATH>.Lib contains the landmark, we are running
      from the build directory.

   3. Each directory from argv0_path upwards is checked for
      python314.lib.os (the landmark).

   4. The PREFIX macro (normally <Python3$Dir>, canonicalised at run time)
      is tried.

   The shared library modules (exec_prefix) are located in the same way,
   except that in the build directory pybuilddir/txt gives their location,
   and the landmark is the python314.lib.lib-dynload directory.

   The module search path is then:
       $PYTHONPATH, the zip file (if it exists), the PYTHONPATH macro entries
       (relative to the prefix), the standard library, lib-dynload.

   sys.prefix and sys.exec_prefix are the installation directory (<dir>).
*/

#include "Python.h"
#include "pycore_initconfig.h"    // _PyStatus_ERR()
#include "pycore_pathconfig.h"    // _PyPathConfig_ReadGlobal()
#include "pycore_pymem.h"         // _PyMem_RawStrdup()
#include "osdefs.h"               // DELIM, SEP, MAXPATHLEN

#include <kernel.h>
#include <swis.h>

#include <string.h>

#if !defined(PREFIX) || !defined(EXEC_PREFIX) || !defined(VPATH) || !defined(PYTHONPATH)
#error "PREFIX, EXEC_PREFIX, VPATH and PYTHONPATH must be defined"
#endif

#ifndef LANDMARK
#  define LANDMARK "os"
#endif

/* Filetypes */
#define FILETYPE_PYTHON   0xa73
#define FILETYPE_PYCODE   0xa74
#define FILETYPE_ZIP      0xa91

#define PATHLEN_ERR() _PyStatus_ERR("path configuration: path too long")

#define BUFLEN (MAXPATHLEN + 1)

typedef struct {
    char *home;                /* PYTHONHOME, or NULL */
    char *prefix_macro;        /* PREFIX macro, canonicalised */
    char *exec_prefix_macro;   /* EXEC_PREFIX macro, canonicalised */
    char lib_python[32];       /* "python314.lib" */
    int warnings;

    char argv0_path[BUFLEN];   /* directory containing the executable */
    char prefix[BUFLEN];       /* <dir>.python314.lib */
    char exec_prefix[BUFLEN];  /* <dir>.python314.lib.lib-dynload */
    int prefix_found;          /* 1 installed, -1 build dir, 0 not found */
    int exec_prefix_found;
} PyCalculatePath;


/* --- RISC OS filing system helpers -------------------------------------- */

/* Remove the last component of a path */
static void
reduce(char *dir)
{
    char *sep = strrchr(dir, SEP);
    if (sep != NULL) {
        *sep = '\0';
    }
    else {
        dir[0] = '\0';
    }
}

/* Canonicalise a path with OS_FSControl 37.  This expands system variables
   such as <Python3$Dir>.  Returns a PyMem_RawMalloc()-ed string, or NULL
   on memory error.  If the path cannot be canonicalised, a copy of it is
   returned. */
static char *
canonicalise(const char *path)
{
    int spare = 0;
    if (_swix(OS_FSControl, _INR(0,5) | _OUT(5),
              37, path, 0, 0, 0, 0, &spare) != NULL) {
        return _PyMem_RawStrdup(path);
    }
    int size = 1 - spare;
    char *res = PyMem_RawMalloc(size);
    if (res == NULL) {
        return NULL;
    }
    if (_swix(OS_FSControl, _INR(0,5),
              37, path, res, 0, 0, size) != NULL) {
        PyMem_RawFree(res);
        return _PyMem_RawStrdup(path);
    }
    return res;
}

/* RISC OS object type: 0 not found, 1 file, 2 directory, 3 image */
static int
get_obj_type(const char *pathname)
{
    int obj_type = 0;
    if (_swix(OS_File, _INR(0,1) | _OUT(0), 17, pathname, &obj_type) != NULL) {
        return 0;
    }
    return obj_type;
}

/* RISC OS filetype, or -1 */
static int
get_filetype(const char *filename)
{
    int obj_type = 0, filetype = -1;
    if (_swix(OS_File, _INR(0,1) | _OUT(0) | _OUT(6),
              23, filename, &obj_type, &filetype) != NULL || obj_type == 0) {
        return -1;
    }
    return filetype;
}

static int
isfile(const char *filename)
{
    return get_obj_type(filename) == 1;
}

static int
isdir(const char *filename)
{
    int obj_type = get_obj_type(filename);
    return obj_type == 2 || obj_type == 3;  /* Directory or image */
}

/* Append a path component to buffer */
static PyStatus
joinpath(char *buffer, const char *stuff, size_t buflen)
{
    size_t n = strlen(buffer);
    size_t k = strlen(stuff);

    if (n > 0 && buffer[n-1] != SEP && k > 0) {
        if (n + 1 >= buflen) {
            return PATHLEN_ERR();
        }
        buffer[n++] = SEP;
    }
    if (n + k >= buflen) {
        return PATHLEN_ERR();
    }
    memcpy(buffer + n, stuff, k + 1);
    return _PyStatus_OK();
}

static int
safe_strcpy(char *dst, const char *src, size_t n)
{
    size_t srclen = strlen(src);
    if (n <= srclen) {
        dst[0] = '\0';
        return -1;
    }
    memcpy(dst, src, srclen + 1);
    return 0;
}

/* Is 'module' in 'directory'?  It may be a typed Python file, or have a
   /py or /pyc suffix. */
static int
ismodule(const char *directory, const char *module)
{
    char buffer[BUFLEN];
    if (safe_strcpy(buffer, directory, BUFLEN) < 0 ||
        _PyStatus_EXCEPTION(joinpath(buffer, module, BUFLEN))) {
        return 0;
    }

    int filetype = get_filetype(buffer);
    if (filetype == FILETYPE_PYTHON || filetype == FILETYPE_PYCODE) {
        return 1;
    }

    size_t n = strlen(buffer);
    if (n + 5 > BUFLEN) {
        return 0;
    }
    strcpy(buffer + n, "/py");
    if (isfile(buffer)) {
        return 1;
    }
    strcpy(buffer + n, "/pyc");
    return isfile(buffer);
}

/* Stop searching upwards at the root of a filing system or path variable,
   or a special directory ($, &, %). */
static int
is_root(const char *path)
{
    size_t len = strlen(path);
    if (len == 0) {
        return 1;
    }
    char last = path[len - 1];
    return last == '$' || last == '&' || last == '%' || last == ':';
}


/* --- prefix (standard library) ---------------------------------------- */

static PyStatus
search_for_prefix(PyCalculatePath *calc)
{
    PyStatus status;
    char *prefix = calc->prefix;

    /* If PYTHONHOME is set, we believe it unconditionally */
    if (calc->home) {
        if (safe_strcpy(prefix, calc->home, BUFLEN) < 0) {
            return PATHLEN_ERR();
        }
        char *delim = strchr(prefix, DELIM);
        if (delim) {
            *delim = '\0';
        }
        calc->prefix_found = 1;
        return joinpath(prefix, calc->lib_python, BUFLEN);
    }

    /* Are we running from the build directory?
       Path: <argv0_path> . <VPATH> . Lib */
    if (safe_strcpy(prefix, calc->argv0_path, BUFLEN) < 0) {
        return PATHLEN_ERR();
    }
    status = joinpath(prefix, VPATH, BUFLEN);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }
    status = joinpath(prefix, "Lib", BUFLEN);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }
    if (ismodule(prefix, LANDMARK)) {
        calc->prefix_found = -1;
        return _PyStatus_OK();
    }

    /* Search upwards from argv0_path */
    if (safe_strcpy(prefix, calc->argv0_path, BUFLEN) < 0) {
        return PATHLEN_ERR();
    }
    while (prefix[0]) {
        size_t n = strlen(prefix);
        status = joinpath(prefix, calc->lib_python, BUFLEN);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
        if (ismodule(prefix, LANDMARK)) {
            calc->prefix_found = 1;
            return _PyStatus_OK();
        }
        prefix[n] = '\0';
        if (is_root(prefix)) {
            break;
        }
        reduce(prefix);
    }

    /* Try the PREFIX macro */
    if (safe_strcpy(prefix, calc->prefix_macro, BUFLEN) < 0) {
        return PATHLEN_ERR();
    }
    status = joinpath(prefix, calc->lib_python, BUFLEN);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }
    calc->prefix_found = ismodule(prefix, LANDMARK) ? 1 : 0;
    return _PyStatus_OK();
}


/* --- exec_prefix (shared library modules) ----------------------------- */

/* pybuilddir/txt is written by the build, and contains the location of
   the shared library modules relative to the build directory. */
static PyStatus
search_pybuilddir(PyCalculatePath *calc)
{
    PyStatus status;
    char filename[BUFLEN];

    if (safe_strcpy(filename, calc->argv0_path, BUFLEN) < 0) {
        return PATHLEN_ERR();
    }
    status = joinpath(filename, "pybuilddir/txt", BUFLEN);
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
    char pybuilddir[BUFLEN];
    size_t n = fread(pybuilddir, 1, BUFLEN - 1, fp);
    fclose(fp);
    pybuilddir[n] = '\0';
    /* Strip trailing whitespace */
    while (n > 0 && (pybuilddir[n-1] == '\n' || pybuilddir[n-1] == '\r' ||
                     pybuilddir[n-1] == ' ')) {
        pybuilddir[--n] = '\0';
    }

    if (safe_strcpy(calc->exec_prefix, calc->argv0_path, BUFLEN) < 0) {
        return PATHLEN_ERR();
    }
    status = joinpath(calc->exec_prefix, pybuilddir, BUFLEN);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }
    calc->exec_prefix_found = -1;
    return _PyStatus_OK();
}

/* Set exec_prefix to <dir> . python314.lib . lib-dynload */
static PyStatus
exec_prefix_from(PyCalculatePath *calc, const char *dir)
{
    PyStatus status;
    if (safe_strcpy(calc->exec_prefix, dir, BUFLEN) < 0) {
        return PATHLEN_ERR();
    }
    status = joinpath(calc->exec_prefix, calc->lib_python, BUFLEN);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }
    return joinpath(calc->exec_prefix, "lib-dynload", BUFLEN);
}

static PyStatus
search_for_exec_prefix(PyCalculatePath *calc)
{
    PyStatus status;

    /* If PYTHONHOME is set, we believe it unconditionally */
    if (calc->home) {
        char *delim = strchr(calc->home, DELIM);
        status = exec_prefix_from(calc, delim ? delim + 1 : calc->home);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
        calc->exec_prefix_found = 1;
        return _PyStatus_OK();
    }

    /* Are we running from the build directory? */
    status = search_pybuilddir(calc);
    if (_PyStatus_EXCEPTION(status) || calc->exec_prefix_found) {
        return status;
    }

    /* Search upwards from argv0_path */
    char dir[BUFLEN];
    if (safe_strcpy(dir, calc->argv0_path, BUFLEN) < 0) {
        return PATHLEN_ERR();
    }
    while (dir[0]) {
        status = exec_prefix_from(calc, dir);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
        if (isdir(calc->exec_prefix)) {
            calc->exec_prefix_found = 1;
            return _PyStatus_OK();
        }
        if (is_root(dir)) {
            break;
        }
        reduce(dir);
    }

    /* Try the EXEC_PREFIX macro */
    status = exec_prefix_from(calc, calc->exec_prefix_macro);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }
    calc->exec_prefix_found = isdir(calc->exec_prefix) ? 1 : 0;
    return _PyStatus_OK();
}


/* --- Setting the PyConfig members ------------------------------------- */

static PyStatus
decode_path(const char *path, wchar_t **wpath)
{
    size_t len;
    *wpath = Py_DecodeLocale(path, &len);
    if (*wpath == NULL) {
        if (len == (size_t)-2) {
            return _PyStatus_ERR("path configuration: cannot decode path");
        }
        return _PyStatus_NO_MEMORY();
    }
    return _PyStatus_OK();
}

static PyStatus
set_config_str(PyConfig *config, wchar_t **member, const char *value)
{
    wchar_t *wvalue;
    PyStatus status = decode_path(value, &wvalue);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }
    status = PyConfig_SetString(config, member, wvalue);
    PyMem_RawFree(wvalue);
    return status;
}

static PyStatus
append_search_path(PyConfig *config, const char *path)
{
    wchar_t *wpath;
    PyStatus status = decode_path(path, &wpath);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }
    status = PyWideStringList_Append(&config->module_search_paths, wpath);
    PyMem_RawFree(wpath);
    return status;
}

/* Append each DELIM separated entry of 'paths'.  Relative entries are
   made relative to 'base', if not NULL. */
static PyStatus
append_search_paths(PyConfig *config, const char *paths, const char *base)
{
    PyStatus status;
    const char *p = paths;
    while (*p) {
        const char *delim = strchr(p, DELIM);
        size_t n = delim ? (size_t)(delim - p) : strlen(p);
        if (n > 0) {
            char entry[BUFLEN];
            entry[0] = '\0';
            /* An entry is relative unless it has a filing system or path
               variable prefix (contains ':') or starts with a special
               directory ($, &, %, @, ^). */
            int relative = (memchr(p, ':', n) == NULL &&
                            strchr("$&%@^", p[0]) == NULL);
            if (relative && base != NULL) {
                if (safe_strcpy(entry, base, BUFLEN) < 0) {
                    return PATHLEN_ERR();
                }
            }
            size_t len = strlen(entry);
            if (len > 0 && entry[len-1] != SEP) {
                if (len + 1 >= BUFLEN) {
                    return PATHLEN_ERR();
                }
                entry[len++] = SEP;
            }
            if (len + n >= BUFLEN) {
                return PATHLEN_ERR();
            }
            memcpy(entry + len, p, n);
            entry[len + n] = '\0';

            status = append_search_path(config, entry);
            if (_PyStatus_EXCEPTION(status)) {
                return status;
            }
        }
        if (!delim) {
            break;
        }
        p = delim + 1;
    }
    return _PyStatus_OK();
}

static PyStatus
calculate_module_search_path(PyCalculatePath *calc, PyConfig *config,
                             const char *install_dir)
{
    PyStatus status;

    /* $PYTHONPATH goes first */
    if (config->pythonpath_env != NULL) {
        char *env = Py_EncodeLocale(config->pythonpath_env, NULL);
        if (env == NULL) {
            return _PyStatus_NO_MEMORY();
        }
        status = append_search_paths(config, env, NULL);
        PyMem_Free(env);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
    }

    /* Then the zipped standard library, if present:
       <dir>.python314/zip, or a zip-typed <dir>.python314 */
    if (install_dir != NULL) {
        char zip_path[BUFLEN];
        char zip_name[32];
        if (safe_strcpy(zip_path, install_dir, BUFLEN) < 0) {
            return PATHLEN_ERR();
        }
        PyOS_snprintf(zip_name, sizeof(zip_name), "python%d%d/zip",
                      PY_MAJOR_VERSION, PY_MINOR_VERSION);
        status = joinpath(zip_path, zip_name, BUFLEN);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
        int found = isfile(zip_path);
        if (!found) {
            zip_path[strlen(zip_path) - 4] = '\0';  /* remove "/zip" */
            found = (get_filetype(zip_path) == FILETYPE_ZIP);
        }
        if (found) {
            status = append_search_path(config, zip_path);
            if (_PyStatus_EXCEPTION(status)) {
                return status;
            }
        }
    }

    /* Then the PYTHONPATH macro entries, relative to the prefix */
    status = append_search_paths(config, PYTHONPATH, calc->prefix);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    /* The standard library */
    status = append_search_path(config, calc->prefix);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    /* Finally, the directory for shared library modules */
    status = append_search_path(config, calc->exec_prefix);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    config->module_search_paths_set = 1;
    return _PyStatus_OK();
}


static PyStatus
calculate_executable(PyCalculatePath *calc, PyConfig *config)
{
    PyStatus status;

    /* As in Modules/getpath.py, program_name defaults to argv[0], or
       "python3" if that is not available. */
    if (config->program_name == NULL || config->program_name[0] == L'\0') {
        const wchar_t *name = L"python3";
        if (config->orig_argv.length > 0 &&
            config->orig_argv.items[0][0] != L'\0') {
            name = config->orig_argv.items[0];
        }
        status = PyConfig_SetString(config, &config->program_name, name);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
    }

    /* argv[0] is normally the full path on RISC OS, but canonicalise it in
       case it is relative or uses a path variable. */
    const wchar_t *wexecutable = config->executable;
    if (wexecutable == NULL) {
        wexecutable = config->program_name;
    }
    char *executable_raw = Py_EncodeLocale(wexecutable, NULL);
    if (executable_raw == NULL) {
        return _PyStatus_NO_MEMORY();
    }
    char *executable = NULL;
    if (config->executable == NULL && !isfile(executable_raw) &&
        strchr(executable_raw, ':') == NULL &&
        strchr(executable_raw, SEP) == NULL &&
        strlen(executable_raw) + 5 <= BUFLEN) {
        /* A bare name, so look for it on Run$Path */
        char run_path[BUFLEN];
        strcpy(run_path, "Run:");
        strcat(run_path, executable_raw);
        if (isfile(run_path)) {
            executable = canonicalise(run_path);
            if (executable == NULL) {
                PyMem_Free(executable_raw);
                return _PyStatus_NO_MEMORY();
            }
        }
    }
    if (executable == NULL) {
        executable = canonicalise(executable_raw);
    }
    PyMem_Free(executable_raw);
    if (executable == NULL) {
        return _PyStatus_NO_MEMORY();
    }

    if (safe_strcpy(calc->argv0_path, executable, BUFLEN) < 0) {
        status = PATHLEN_ERR();
        goto done;
    }
    reduce(calc->argv0_path);

    if (config->executable == NULL) {
        status = set_config_str(config, &config->executable, executable);
        if (_PyStatus_EXCEPTION(status)) {
            goto done;
        }
    }
    if (config->base_executable == NULL) {
        status = set_config_str(config, &config->base_executable, executable);
        if (_PyStatus_EXCEPTION(status)) {
            goto done;
        }
    }
    status = _PyStatus_OK();

done:
    PyMem_RawFree(executable);
    return status;
}


static PyStatus
calculate_path(PyCalculatePath *calc, PyConfig *config)
{
    PyStatus status;

    status = calculate_executable(calc, config);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    /* Find the standard library and shared library modules */
    status = search_for_prefix(calc);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }
    if (!calc->prefix_found) {
        if (calc->warnings) {
            fprintf(stderr,
                    "Could not find platform independent libraries <prefix>\n");
        }
        if (safe_strcpy(calc->prefix, calc->prefix_macro, BUFLEN) < 0) {
            return PATHLEN_ERR();
        }
        status = joinpath(calc->prefix, calc->lib_python, BUFLEN);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
    }

    status = search_for_exec_prefix(calc);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }
    if (!calc->exec_prefix_found) {
        if (calc->warnings) {
            fprintf(stderr,
                    "Could not find platform dependent libraries <exec_prefix>\n");
        }
        status = exec_prefix_from(calc, calc->exec_prefix_macro);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
    }

    if ((!calc->prefix_found || !calc->exec_prefix_found) && calc->warnings) {
        fprintf(stderr, "Searched from %s\n", calc->argv0_path);
        fprintf(stderr,
                "Consider setting PYTHONHOME to <prefix>[,<exec_prefix>]\n");
    }

    /* sys.prefix and sys.exec_prefix are the installation directory, so
       strip "python314.lib" (and ".lib-dynload").  In the build directory
       the macros are used instead. */
    char prefix_dir[BUFLEN], exec_prefix_dir[BUFLEN];
    if (calc->prefix_found > 0) {
        safe_strcpy(prefix_dir, calc->prefix, BUFLEN);
        reduce(prefix_dir);
        reduce(prefix_dir);
    }
    else if (safe_strcpy(prefix_dir, calc->prefix_macro, BUFLEN) < 0) {
        return PATHLEN_ERR();
    }
    if (calc->exec_prefix_found > 0) {
        safe_strcpy(exec_prefix_dir, calc->exec_prefix, BUFLEN);
        reduce(exec_prefix_dir);
        reduce(exec_prefix_dir);
        reduce(exec_prefix_dir);
    }
    else if (safe_strcpy(exec_prefix_dir, calc->exec_prefix_macro, BUFLEN) < 0) {
        return PATHLEN_ERR();
    }

    if (!config->module_search_paths_set) {
        status = calculate_module_search_path(
            calc, config, calc->prefix_found > 0 ? prefix_dir : NULL);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
    }

#define SET_IF_UNSET(MEMBER, VALUE) \
    do { \
        if (config->MEMBER == NULL) { \
            status = set_config_str(config, &config->MEMBER, (VALUE)); \
            if (_PyStatus_EXCEPTION(status)) { \
                return status; \
            } \
        } \
    } while (0)

    SET_IF_UNSET(prefix, prefix_dir);
    SET_IF_UNSET(base_prefix, prefix_dir);
    SET_IF_UNSET(exec_prefix, exec_prefix_dir);
    SET_IF_UNSET(base_exec_prefix, exec_prefix_dir);
    SET_IF_UNSET(stdlib_dir, calc->prefix);

#undef SET_IF_UNSET

    return _PyStatus_OK();
}


/* Calculate the path configuration.

   When compute_path_config is 0, this only reads any initialised path
   config values into the PyConfig struct, for example, Py_SetPythonHome().

   Values already set in the config are not changed. */
PyStatus
_PyConfig_InitPathConfig(PyConfig *config, int compute_path_config)
{
    PyStatus status = _PyPathConfig_ReadGlobal(config);
    if (_PyStatus_EXCEPTION(status) || !compute_path_config) {
        return status;
    }

    PyCalculatePath calc;
    memset(&calc, 0, sizeof(calc));
    calc.warnings = config->pathconfig_warnings;
    PyOS_snprintf(calc.lib_python, sizeof(calc.lib_python), "python%d%d%clib",
                  PY_MAJOR_VERSION, PY_MINOR_VERSION, SEP);

    if (config->home != NULL) {
        char *home = Py_EncodeLocale(config->home, NULL);
        if (home == NULL) {
            status = _PyStatus_NO_MEMORY();
            goto done;
        }
        calc.home = _PyMem_RawStrdup(home);
        PyMem_Free(home);
        if (calc.home == NULL) {
            status = _PyStatus_NO_MEMORY();
            goto done;
        }
    }

    calc.prefix_macro = canonicalise(PREFIX);
    calc.exec_prefix_macro = canonicalise(EXEC_PREFIX);
    if (calc.prefix_macro == NULL || calc.exec_prefix_macro == NULL) {
        status = _PyStatus_NO_MEMORY();
        goto done;
    }

    status = calculate_path(&calc, config);

done:
    PyMem_RawFree(calc.home);
    PyMem_RawFree(calc.prefix_macro);
    PyMem_RawFree(calc.exec_prefix_macro);
    return status;
}


/* Used by _testinternalcapi to run Modules/getpath.py, which is not used
   on RISC OS */
PyObject *
_Py_Get_Getpath_CodeObject(void)
{
    PyErr_SetString(PyExc_NotImplementedError,
                    "getpath.py is not used on RISC OS");
    return NULL;
}
