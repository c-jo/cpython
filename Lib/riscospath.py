"""Common operations on RISC OS pathnames.

Instead of importing this module directly, import os and refer to this
module as os.path.
"""

# Strings representing various path-related bits and pieces.
# These are primarily for export; internally, they are hardcoded.
# Should be set before imports for resolving cyclic dependency.
curdir = ''
pardir = '^'
extsep = '/'
sep = '.'
pathsep = ','
defpath = ''
altsep = None
devnull = 'null:'

_absolutes = '$ & % \\ @'.split()

import os
import sys
import stat
import genericpath
from genericpath import *

__all__ = ["normcase","isabs","join","splitdrive","split","splitext",
           "basename","dirname","commonprefix","getsize","getmtime",
           "getatime","getctime","islink","exists","lexists","isdir","isfile",
           "ismount", "expanduser","expandvars","normpath","abspath",
           "curdir","pardir","sep","pathsep","defpath","altsep",
           "extsep","devnull","realpath","supports_unicode_filenames","relpath",
           "samefile","sameopenfile","samestat","commonpath","canonicalise"]


def _get_sep(path):
    if isinstance(path, bytes):
        return b'.'
    else:
        return '.'

# Decode various parts of a path - the returned tuple contains the following
# parts, any of which can be empty
# ( filesystem,        -- Filesytem part, eg, SDFS,Net,ADFS,Resources etc.
#   special field,     -- #0.254
#   disc specifier,    -- Disc name of number (:RISCOSPi, :0)
#   absolute,          -- Start of the tree, most commonly $ but also &,%,\,@
#   path               -- Rest of the path
# )
def explode(path):
    p = os.fspath(path)
    p1 = p
    fs   = None
    sf   = None
    disc = None
    abs  = None
    path = None

    if len(p) > 0 and p[0] == '#':
        c = p.find(':')
        if c < 0:
            sf = p[1:]
            p  = ''
        else:
            sf = p[1:c]
            p  = p[c+1:]

    # Find the filesystem and/or disc specifier
    c = p.find(':')
    if c == 0: # Starts with a disc specification
        d = p.find('.',0) # No end of disc spec, so everything is the drive
        if d < 0:
            disc = p[1:]
        else:
            disc = p[1:d+1]
            path = p[d+1:]

    if c > 0: # Starts with a filing system
        fs = p[:c]
        if c < len(p)-1 and p[c+1] == ':': # Found disc spec too
            d = p.find('.',c)
            if d < 0: # No end of disc spec
                disc = p[c+1]
            else:
                disc = p[c+2:d]
                path = p[d+1:]

        else: # No disc spec
            path = p[c+1:]

    if c < 0:
        path = p
        
    # If the fs contains a special field then extract that
    if fs and '#' in fs:
        h = fs.find('#')
        sf = fs[h+1:]
        fs = fs[:h]

    # If the path starts with an absolute then etract that
    if path and path.split('.')[0] in _absolutes:
        abs  = path[0]
        path = path[2:]

    if path is not None and len(path) == 0:
        path = None

    #print(f"Exploding {p1} -> {(fs,sf,disc,abs,path)}")
    return (fs,sf,disc,abs,path)

# Normalize the case of a pathname.  Trivial in Posix, string.lower on Mac
# and RISC OS. On MS-DOS this may also turn slashes into backslashes; however,
# other normalizations (such as optimizing '../' away) are not allowed
# (another function should be defined to do that).

def normcase(s):
    """Normalize case of pathname."""
    return os.fspath(s).lower()


# Return whether a path is absolute.
# Having removed the 'drive', see if the first element is one of the absolutes.
def isabs(s):
    """Test whether a path is absolute"""
    drive,path = splitdrive(s)
    return path.split('.')[0] in _absolutes

# Join pathnames.
# Ignore the previous parts if a part is absolute.
# Insert a '.' unless the first part is empty or already ends in '.'.

def join(a, *p):
    """Join two or more pathname components, inserting '.' as needed.
    handle file system, special field, disc specifier and absolutes."""
    a = os.fspath(a)
    #print("a",a,explode(a))
    sep = _get_sep(a)
    fs,sf,disc,abs,path = explode(a)
    try:
        if not p:
            return a

        for b in map(os.fspath, p):
            if b == '..':
                b = '^'
            fs_,sf_,disc_,abs_,path_ = explode(b)
            if fs_:
                fs   = fs_
                sf   = sf_
                disc = disc_
                abs  = abs_
                path = path_
            else:
                if sf_:
                    sf = sf_
                if disc_:
                    disc = disc_
                    abs  = None
                    path = None
                if abs_:
                    abs  = abs_
                    path = None
                if path_:
                    if path:
                        path = path + '.' + path_
                    else:
                        path = path_

            #if b.startswith(sep):
            #    path = b
            #elif not path or path.endswith(sep):
            #    path += b
            #else:
            #    path += sep + b
    except (TypeError, AttributeError, BytesWarning):
        genericpath._check_arg_types('join', a, *p)
        raise
    x = ''
    if fs:
        x += fs
    if sf:
        x += '#'+sf
    if fs or sf:
        x += ':'
    if disc:
        x += ':'+disc
    if abs:
        if len(x) > 0 and x[-1] != ':':
            x += '.'
        x += abs
    if path:
        #print("*",path,normpath(path))
        #path = normpath(path)
        if len(path) > 0:
            if len(x) > 0 and x[-1] != ':':
                x += '.'
            x += path
    # print("=",x)
    return x # path


# Split a path in head (everything up to the last '.') and tail (the
# rest).  If the path ends in '.', tail will be empty.  If there is no
# '.' in the path, head  will be empty.
# Trailing '.'es are stripped from head unless it is the root.

def split(p):
    """Split a pathname.  Returns tuple "(head, tail)" where "tail" is
    everything after the final slash.  Either part may be empty."""
    p = os.fspath(p)
    sep = _get_sep(p)
    i = p.rfind(sep) + 1
    head, tail = p[:i], p[i:]
    if head and head != sep*len(head):
        head = head.rstrip(sep)
    return head, tail


# Split a path in root and extension.
# The extension is everything starting at the last dot in the last
# pathname component; the root is everything before that.
# It is always true that root + ext == p.

def splitext(p):
    p = os.fspath(p)
    if isinstance(p, bytes):
        sep = b'.'
        extsep = b'/'
    else:
        sep = '.'
        extsep = '/'
    return genericpath._splitext(p, sep, None, extsep)
splitext.__doc__ = genericpath._splitext.__doc__

# Split a pathname into a drive specification and the rest of the
# path.
# It is always true that drivespec + pathspec == p
# On RISC OS the drive is the filing system, special field and discname.
def splitdrive(p):
    """Split a pathname into drive name and relative path specifiers.
    Returns a 2-tuple (drive, path); either part may be empty.

    If you assign
        result = splitdrive(p)
    It is always true that:
        result[0] + result[1] == p

    On RISC OS, we se drive to mean the filesystem, special fields and media
    descriptior. Eg. Net#0.254::Server., ADFS:, SDFS::RISCOSPi. In order to
    satisfy the condtion above, the drive may include a trailing . (marking the
    end of disc specification).
    """
    p = os.fspath(p)

    pos = p.find(':')
    if pos == 0: # Starts with a disc specification
        pos = p.find('.',0) # No end of disc spec, so everything is the drive
        if pos < 0:
            return p,''

        return p[0:pos+1],p[pos+1:]

    if pos > 0: # Starts with a filing system
        if pos < len(p)-1 and p[pos+1] == ':': # Found disc spec too
            pos = p.find('.',pos)
            if pos < 0: # No end of disc spec, so everything is the drive
                return p,''
            else:
                return p[0:pos+1],p[pos+1:]

        else: # No disc spec
            return p[0:pos+1],p[pos+1:]

    return '',p

# Return the tail (basename) part of a path, same as split(path)[1].

def basename(p):
    """Returns the final component of a pathname"""
    p = os.fspath(p)
    sep = _get_sep(p)
    i = p.rfind(sep) + 1
    return p[i:]


# Return the head (dirname) part of a path, same as split(path)[0].

def dirname(p):
    """Returns the directory component of a pathname"""
    p = os.fspath(p)
    sep = _get_sep(p)
    i = p.rfind(sep) + 1
    head = p[:i]
    if head and head != sep*len(head):
        head = head.rstrip(sep)
    return head


# Is a path a symbolic link?
# This will always return false on systems where os.lstat doesn't exist.

def islink(path):
    """Test whether a path is a symbolic link"""
    try:
        st = os.lstat(path)
    except (OSError, ValueError, AttributeError):
        return False
    return stat.S_ISLNK(st.st_mode)

# Being true for dangling symbolic links is also useful.

def lexists(path):
    """Test whether a path exists.  Returns True for broken symbolic links"""
    try:
        os.lstat(path)
    except (OSError, ValueError):
        return False
    return True


# Is a path a mount point?
# For RISC OS we, a file system and discname or absolute
# must be specified, without any further path.

def ismount(path):
    """Test whether a path is a mount point."""
    fs,sf,disc,abs,path = explode(os.fspath(path))
    return fs and (disc or abs) and not path


# Expand paths beginning with '~' or '~user'.
# '~' means $HOME; '~user' means that user's home directory.
# If the path doesn't begin with '~', or if the user or $HOME is unknown,
# the path is returned unchanged (leaving error reporting to whatever
# function is called with the expanded path as argument).
# See also module 'glob' for expansion of *, ? and [...] in pathnames.
# (A function should also be defined to do full *sh-style environment
# variable expansion.)

def expanduser(path):
    # Don't really have the concept on RISC OS, except & for the URD
    # but that doesn't get expanded. Select has the users thing, maybe
    # use that if it's there?
    return path
 
# Expand paths containing shell variable substitutions.
# This expands the forms $variable and ${variable} only.
# Non-existent variables are left unchanged.

_varprog = None
_varprogb = None

def expandvars(path):
    """Expand shell variables of form $var and ${var}.  Unknown variables
    are left unchanged."""
    path = os.fspath(path)
    global _varprog, _varprogb
    if isinstance(path, bytes):
        if b'$' not in path:
            return path
        if not _varprogb:
            import re
            _varprogb = re.compile(br'\$(\w+|\{[^}]*\})', re.ASCII)
        search = _varprogb.search
        start = b'{'
        end = b'}'
        environ = getattr(os, 'environb', None)
    else:
        if '$' not in path:
            return path
        if not _varprog:
            import re
            _varprog = re.compile(r'\$(\w+|\{[^}]*\})', re.ASCII)
        search = _varprog.search
        start = '{'
        end = '}'
        environ = os.environ
    i = 0
    while True:
        m = search(path, i)
        if not m:
            break
        i, j = m.span(0)
        name = m.group(1)
        if name.startswith(start) and name.endswith(end):
            name = name[1:-1]
        try:
            if environ is None:
                value = os.fsencode(os.environ[os.fsdecode(name)])
            else:
                value = environ[name]
        except KeyError:
            i = j
        else:
            tail = path[j:]
            path = path[:i] + value
            i = len(path)
            path += tail
    return path


# Normalize a path, e.g. A.B, A.foo.^.B all become A.B.
# If we wanted we could probably convert the archaic -adfs- type specifiers
# here, but we currently don't.

def normpath(path):
    """Normalize path."""
    path = os.fspath(path)
    if isinstance(path, bytes):
        sep = b'.'
        curdir = b''
        pardir = b'^'
    else:
        sep = '.'
        curdir = ''
        pardir = '^'

    prefix, path = splitdrive(path)
    #print(prefix)

    if len(path) == 0: # No path
        return prefix

    comps = path.split(sep)
    s = 0
    if len(comps) > 0 and comps[0] in _absolutes:
        s = 1
    i = s
    #print(comps,i,s)
    while i < len(comps):
        #print(f" {i} '{comps[i]}'")
        if not comps[i]:
            del comps[i]
        elif comps[i] == pardir:
            if i > s and comps[i-1] != pardir:
                del comps[i-1:i+1]
                i -= 1
            elif i == s == 1:
                del comps[i]
            else:
                i += 1
        else:
            i += 1
    # If the path is now empty, substitute '.'
    #if not prefix and not comps:
    #    comps.append(curdir)
    return prefix + sep.join(comps)


def abspath(path):
    """Return an absolute path."""
    path = os.fspath(path)
    if not isabs(path):
        if isinstance(path, bytes):
            cwd = os.getcwdb()
        else:
            cwd = os.getcwd()
        path = join(cwd, path)
    return normpath(path)


# Return a canonical path (i.e. the absolute location of a file on the
# filesystem).


# realpath is a no-op on systems without islink support
realpath = abspath

# RISC OS has no Unicode filename support.
supports_unicode_filenames = False

def relpath(path, start=None):
    """Return a relative version of a path"""
    #print('relpath',path,start)
    if not path:
        raise ValueError("no path specified")

    # Check they have same fs, sf, disc and absolute
    path = os.fspath(path)
    if isinstance(path, bytes):
        curdir = b''
        sep = b'.'
        pardir = b'^'
    else:
        curdir = ''
        sep = '.'
        pardir = '^'

    if start is None:
        start = os.fspath(curdir)
    else:
        start = os.fspath(start)

    try:
        start_abs = abspath(normpath(start))
        path_abs = abspath(normpath(path))
        start_drive, start_rest = splitdrive(start_abs)
        path_drive, path_rest = splitdrive(path_abs)
        if normcase(start_drive) != normcase(path_drive):
            raise ValueError("path is on %r, start on %r" % (
                path_drive, start_drive))

        start_list = [x for x in start_rest.split(sep) if x]
        path_list = [x for x in path_rest.split(sep) if x]
        #print(start_list,path_list)
        if len(start_list) > 0 and len(path_list) > 0:
            start_first = start_list[0]
            path_first = path_list[0]
            if start_first in _absolutes and \
                path_first in _absolutes and \
                start_first != path_first:
                raise ValueError("path from %r, start is from %r" % (
                    path_first, start_first))
        # Work out how much of the filepath is shared by start and path.
        i = 0
        for e1, e2 in zip(start_list, path_list):
            if normcase(e1) != normcase(e2):
                break
            i += 1

        rel_list = [pardir] * (len(start_list)-i) + path_list[i:]
        if not rel_list:
            return curdir
        return join(*rel_list)
    except (TypeError, ValueError, AttributeError, BytesWarning, DeprecationWarning):
        genericpath._check_arg_types('relpath', path, start)
        raise


# Return the longest common sub-path of the sequence of paths given as input.
# The paths are not normalized before comparing them (this is the
# responsibility of the caller). Any trailing separator is stripped from the
# returned path.

def commonpath(paths):
    """Given a sequence of path names, returns the longest common sub-path."""

    if not paths:
        raise ValueError('commonpath() arg is an empty sequence')

    paths = tuple(map(os.fspath, paths))
    if isinstance(paths[0], bytes):
        sep = b'.'
        curdir = b''
    else:
        sep = '.'
        curdir = ''

    try:
        split_paths = [path.split(sep) for path in paths]

        try:
            isabs, = set(p[:1] == sep for p in paths)
        except ValueError:
            raise ValueError("Can't mix absolute and relative paths") from None

        split_paths = [[c for c in s if c and c != curdir] for s in split_paths]
        s1 = min(split_paths)
        s2 = max(split_paths)
        common = s1
        for i, c in enumerate(s1):
            if c != s2[i]:
                common = s1[:i]
                break

        prefix = sep if isabs else sep[:0]
        return prefix + sep.join(common)
    except (TypeError, AttributeError):
        genericpath._check_arg_types('commonpath', *paths)
        raise

def canonicalise(path):
    import swi
    buffer = swi.block(256)
    swi.swi('OS_FSControl','isbiii',37,path,buffer,0,0,256*4)
    return buffer.nullstring()
