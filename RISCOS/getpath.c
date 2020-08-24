/* Return the initial module search path - RISC OS version */

#include "Python.h"
#include "pycore_initconfig.h"
#include "osdefs.h"
#include "pycore_fileutils.h"
#include "pycore_pathconfig.h"
#include "pycore_pystate.h"

#include "oslib/os.h"

#include <sys/types.h>
#include <string.h>

/* We need two directories, the platform independent directory (prefix),
 * containing the common .py and .pyc files, and the platform dependent
 * directory (exec_prefix), containing the shared library modules. Note that
 * prefix and exec_prefix can be the same directory, but for some installations,
 * they are different.
 *
 * Rather than searching (given they may be on a different filing system) we
 * use ene environment variables PythonX.Y$Prefix and PythonX.Y$ExecPrefix
 * to get these. !Python3.!Boot will set them up when the application is 'seen'
 * by the filer.
 *
 * We also use the location of the executable. RISC OS appears to give us the
 * full path in argv[0], and argv0_path is set to the directory containing the
 * executable (i.e. the last component is stripped).
 *
 * Are we running python out of the build directory? This is checked by looking
 * for a landmark relative to argv0_path. For prefix, the landmark's path is
 * derived from the VPATH
 * preprocessor variable (taking into account that its value is almost, but
 * not quite, what we need). For exec_prefix, the landmark is pybuilddir. If the landmark is found, this overrides the ...$Prefix and ...$ExecPrefix variables above.
 *
 * These are used to create the path, and the contents of Python3$Path (if set)
 * are inserted in front of it.
 *
 * An embedding application can use Py_SetPath() to override all of
 * these authomatic path computations.
 */


#ifdef __cplusplus
extern "C" {
#endif

#include <swis.h>

#if !defined(VERSION)
#error "VERSION must be constant defined"
#endif

/*
#ifndef LANDMARK
#define LANDMARK L"os.py"
#endif
*/

#define DECODE_LOCALE_ERR(NAME, LEN) \
    ((LEN) == (size_t)-2) \
     ? _PyStatus_ERR("cannot decode " NAME) \
     : _PyStatus_NO_MEMORY()

#define PATHLEN_ERR() _PyStatus_ERR("path configuration: path too long")

typedef struct {
    wchar_t *pythonpath;    /* <Python3$Path>      environment variable */
    wchar_t *prefix;        /* <PythonX.Y$Prefix>  environment variable */
    wchar_t *exec_prefix;   /* <PythonX.Y$DPrefix> environment variable */
    wchar_t argv0_path[MAXPATHLEN+1];
} PyCalculatePath;

static const wchar_t delimiter[2] = {DELIM, '\0'};
static const wchar_t separator[2] = {SEP,   '\0'};

/* Get file status. Encode the path to the locale encoding. */
static int
_Py_wstat(const wchar_t* path, struct stat *buf)
{
    int err;
    char *fname;
    fname = _Py_EncodeLocaleRaw(path, NULL);
    if (fname == NULL) {
        errno = EINVAL;
        return -1;
    }
    err = stat(fname, buf);
    PyMem_RawFree(fname);
    return err;
}


static void
reduce(wchar_t *dir)
{
    size_t i = wcslen(dir);
    while (i > 0 && dir[i] != SEP)
        --i;
    dir[i] = '\0';
}


static int
isfile(wchar_t *filename)          /* Is file, not directory */
{
    struct stat buf;
    if (_Py_wstat(filename, &buf) != 0) {
        return 0;
    }
    if (!S_ISREG(buf.st_mode)) {
        return 0;
    }
    return 1;
}

/* Add a path component, by appending stuff to buffer.
   buffer must have at least MAXPATHLEN + 1 bytes allocated, and contain a
   NUL-terminated string with no more than MAXPATHLEN characters (not counting
   the trailing NUL).  It's a fatal error if it contains a string longer than
   that (callers must be careful!).  If these requirements are met, it's
   guaranteed that buffer will still be a NUL-terminated string with no more
   than MAXPATHLEN characters at exit.  If stuff is too long, only as much of
   stuff as fits will be appended.
*/
static void
joinpath(wchar_t *buffer, wchar_t *stuff)
{
    size_t n, k;
    if (stuff[0] == SEP) {
        n = 0;
    }
    else {
        n = wcslen(buffer);
        if (n > 0 && buffer[n-1] != SEP && n < MAXPATHLEN) {
            buffer[n++] = SEP;
        }
    }
    if (n > MAXPATHLEN) {
        Py_FatalError("buffer overflow in getpath.c's joinpath()");
    }
    k = wcslen(stuff);
    if (n + k > MAXPATHLEN) {
        k = MAXPATHLEN - n;
    }
    wcsncpy(buffer+n, stuff, k);
    buffer[n+k] = '\0';

}

static inline int
safe_wcscpy(wchar_t *dst, const wchar_t *src, size_t n)
{
    size_t srclen = wcslen(src);
    if (n <= srclen) {
        dst[0] = L'\0';
        return -1;
    }
    memcpy(dst, src, (srclen + 1) * sizeof(wchar_t));
    return 0;
}

static PyStatus
calculate_program_full_path(PyCalculatePath *calculate,
                            _PyPathConfig *pathconfig)
{
    wchar_t program_full_path[MAXPATHLEN+1];
    memset(program_full_path, 0, sizeof(program_full_path));
    wcsncpy(program_full_path, pathconfig->program_name, MAXPATHLEN);

    pathconfig->program_full_path = _PyMem_RawWcsdup(program_full_path);
    if (pathconfig->program_full_path == NULL) {
        return _PyStatus_NO_MEMORY();
    }
    return _PyStatus_OK();
}


static PyStatus
calculate_argv0_path(PyCalculatePath *calculate,
                     const wchar_t *program_full_path)
{
    if (safe_wcscpy(calculate->argv0_path, program_full_path,
                    Py_ARRAY_LENGTH(calculate->argv0_path)) < 0)
        return PATHLEN_ERR();

    reduce(calculate->argv0_path);

    /* At this point, argv0_path is guaranteed to be less than
       MAXPATHLEN bytes long. */
    return _PyStatus_OK();
}

/* Checks for pybuilddir in the same directory as argv[0].
If found it will replace calculate->dynload_path to the contents of the file,
otherwise it will leave it alone.
*/
static void
process_pybuilddir(PyCalculatePath *calculate)
{
    /* Check to see if argv[0] is in the build directory. "pybuilddir/txt"
       is written by setup.py and contains the relative path to the location
       of shared library modules. */
    wchar_t pybuilddir[MAXPATHLEN+1];
    wcsncpy(pybuilddir, calculate->argv0_path, MAXPATHLEN);
    pybuilddir[MAXPATHLEN] = L'\0';
    joinpath(pybuilddir, L"pybuilddir/txt");
    if (isfile(pybuilddir)) {
        FILE *f = _Py_wfopen(pybuilddir, L"rb");
        if (f == NULL) {
            errno = 0;
        }
        else {
            char buf[MAXPATHLEN+1];
            wchar_t *rel_builddir_path;
            int n = fread(buf, 1, MAXPATHLEN, f);
            buf[n] = '\0';
            fclose(f);
            rel_builddir_path = _Py_DecodeUTF8_surrogateescape(buf, n, NULL);
            if (rel_builddir_path) {
                wchar_t builddir_path[MAXPATHLEN+1];
                wcsncpy(builddir_path, calculate->argv0_path, MAXPATHLEN);
                builddir_path[MAXPATHLEN] = L'\0';
                joinpath(builddir_path, rel_builddir_path);
                PyMem_RawFree(rel_builddir_path );

                PyMem_RawFree(calculate->exec_prefix);
                calculate->exec_prefix = PyMem_RawMalloc(
                    (wcslen(builddir_path)+1) * sizeof(wchar_t));
                wcscpy(calculate->exec_prefix, builddir_path);
            }
        }
    }
}

static PyStatus
calculate_module_search_path(PyCalculatePath *calculate,
                             _PyPathConfig *pathconfig)
{
    /* Calculate size of return buffer */
    size_t bufsz = 0;

    if (calculate->pythonpath)
        bufsz += wcslen(calculate->pythonpath ) + 1; // +seperator

    bufsz += wcslen(calculate->prefix     ) + 1; // +seperator
    bufsz += wcslen(calculate->exec_prefix) + 1; // +null term.

    /* Allocate the buffer */
    wchar_t *buf = PyMem_RawMalloc(bufsz * sizeof(wchar_t));
    if (buf == NULL) {
        return _PyStatus_NO_MEMORY();
    }
    buf[0] = '\0';

    if (calculate->pythonpath)
    {
        wcscat(buf, calculate->pythonpath);
        if (wcslen(buf) > 0)
        {
            wchar_t last = buf[wcslen(buf)-1];
            if (last != '\0' && last != delimiter[0])
              wcscat(buf, delimiter);
        }
    }
    wcscat(buf, calculate->prefix);
    wcscat(buf, delimiter);
    wcscat(buf, calculate->exec_prefix);

    pathconfig->module_search_path = buf;
    return _PyStatus_OK();
}

static PyStatus
calculate_init(PyCalculatePath *calculate, const PyConfig *config)
{
    size_t len;
    char *_pythonpath  = getenv("Python3$Path");

    if (_pythonpath)
        calculate->pythonpath = Py_DecodeLocale(_pythonpath, &len);
    else
        calculate->pythonpath = 0;
    calculate->prefix      = Py_DecodeLocale(
        getenv("Python"VERSION"$Prefix"), &len);
    calculate->exec_prefix = Py_DecodeLocale(
    getenv("Python"VERSION"$ExecPrefix"), &len);

    return _PyStatus_OK();
}

static void
calculate_free(PyCalculatePath *calculate)
{
    if (calculate->pythonpath)
        PyMem_RawFree(calculate->pythonpath);
    PyMem_RawFree(calculate->prefix     );
    PyMem_RawFree(calculate->exec_prefix);
}


static PyStatus
calculate_path(PyCalculatePath *calculate, _PyPathConfig *pathconfig)
{
    PyStatus status;

    if (pathconfig->program_full_path == NULL) {
        status = calculate_program_full_path(calculate, pathconfig);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
    }

    status = calculate_argv0_path(calculate, pathconfig->program_full_path);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    process_pybuilddir(calculate);

    if (pathconfig->module_search_path == NULL) {
        status = calculate_module_search_path(calculate, pathconfig);//,
                                              //prefix, exec_prefix, zip_path);
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

    pathconfig->prefix      = _PyMem_RawWcsdup(calculate.prefix     );
    pathconfig->exec_prefix = _PyMem_RawWcsdup(calculate.exec_prefix);

    status = _PyStatus_OK();

    done:
    calculate_free(&calculate);
/*
    printf("_PyPathConfig_Calculate returning %d\n", status);
    printf("program_full_path : %ls\n", pathconfig->program_full_path);
    printf("prefix : %ls\n", pathconfig->prefix);
    printf("exec_prefix : %ls\n", pathconfig->exec_prefix);
    printf("module_search_path : %ls\n", pathconfig->module_search_path);
    printf("program_name : %ls\n", pathconfig->program_name);
    printf("home : %ls\n", pathconfig->home);
*/
    return status;
}

#ifdef __cplusplus
}
#endif
