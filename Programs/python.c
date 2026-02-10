/* Minimal main program -- everything is loaded from the library */

#include "Python.h"

#ifdef RISCOS
#include <unixlib/local.h>
int __riscosify_control =  __RISCOSIFY_NO_PROCESS |
                           __RISCOSIFY_NO_SUFFIX |
                           __RISCOSIFY_NO_REVERSE_SUFFIX;
#endif

#ifdef MS_WINDOWS
int
wmain(int argc, wchar_t **argv)
{
    return Py_Main(argc, argv);
}
#else
int
main(int argc, char **argv)
{
    return Py_BytesMain(argc, argv);
}
#endif
