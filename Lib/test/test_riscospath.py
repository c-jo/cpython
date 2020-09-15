import os
import riscospath
import sys
import unittest
import warnings
from test.support import TestFailed, FakePath
from test import support, test_genericpath
from tempfile import TemporaryFile

def _norm(path):
    if isinstance(path, (bytes, str, os.PathLike)):
        return riscospath.normcase(os.fsdecode(path))
    elif hasattr(path, "__iter__"):
        return tuple(riscospath.normcase(os.fsdecode(p)) for p in path)
    return path


def tester(fn, wantResult):
    gotResult = eval(fn)
    if wantResult != gotResult and _norm(wantResult) != _norm(gotResult):
        raise TestFailed("%s should return: %s but returned: %s" \
              %(str(fn), str(wantResult), str(gotResult)))

    """
    # then with bytes
    fn = fn.replace("('", "(b'")
    fn = fn.replace('("', '(b"')
    fn = fn.replace("['", "[b'")
    fn = fn.replace('["', '[b"')
    fn = fn.replace(", '", ", b'")
    fn = fn.replace(', "', ', b"')
    fn = os.fsencode(fn).decode('latin1')
    fn = fn.encode('ascii', 'backslashreplace').decode('ascii')
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", DeprecationWarning)
        gotResult = eval(fn)
    if _norm(wantResult) != _norm(gotResult):
        raise TestFailed("%s should return: %s but returned: %s" \
              %(str(fn), str(wantResult), repr(gotResult)))
    """

class RiscospathTestCase(unittest.TestCase):
    def assertPathEqual(self, path1, path2):
        if path1 == path2 or _norm(path1) == _norm(path2):
            return
        self.assertEqual(path1, path2)

    def assertPathIn(self, path, pathset):
        self.assertIn(_norm(path), _norm(pathset))

class TestRiscospathExtra(RiscospathTestCase):
    def test_explode(self):
        self.assertEqual(riscospath.explode("SDFS::RISCOSPi.$.Work") ,
                ('SDFS', None, 'RISCOSPi', '$', 'Work'))
        self.assertEqual(riscospath.explode("SDFS:$.Work"),
                ('SDFS', None, None, '$', 'Work'))
        self.assertEqual(riscospath.explode("SDFS::RISCOSPi.Work"),
                ('SDFS', None, 'RISCOSPi', None, 'Work'))
        self.assertEqual(riscospath.explode("SDFS::RISCOSPi.$"),
                ('SDFS', None, 'RISCOSPi', '$', None))
        self.assertEqual(riscospath.explode(":RISCOSPi.$.Work"),
                (None, None, 'RISCOSPi.', '$', 'Work'))
        self.assertEqual(riscospath.explode("Net#0.254::Rachel.&.Spam"),
                ('Net', '0.254', 'Rachel', '&', 'Spam'))
        self.assertEqual(riscospath.explode("Net#0.254:&.Spam"),
                ('Net', '0.254', None, '&', 'Spam'))
        self.assertEqual(riscospath.explode("^"),
                (None, None, None, None, '^'))
        self.assertEqual(riscospath.explode("#0.254"),
                (None, "0.254", None, None, None))
        self.assertEqual(riscospath.explode(":Spam"),
                (None, None, 'Spam', None, None))
        self.assertEqual(riscospath.explode("#0.254:&"),
                (None, '0.254', None, '&', None))
        self.assertEqual(riscospath.explode("#0.254::Rachel"),
                (None, '0.254', 'Rachel', None, None))
        self.assertEqual(riscospath.explode(""),
                (None, None, None, None, None))

class TestRiscospath(RiscospathTestCase):
    def test_splitext(self):
        self.assertEqual(riscospath.splitext("foo/ext"),
                ('foo', '/ext'))
        self.assertEqual(riscospath.splitext("$.foo.foo/ext"),
                ('$.foo.foo', '/ext'))
        self.assertEqual(riscospath.splitext("/ext"),
                ('/ext', ''))
        self.assertEqual(riscospath.splitext("foo/ext/"),
                ('foo/ext', '/'))
        self.assertEqual(riscospath.splitext(""),
                ('', ''))
        self.assertEqual(riscospath.splitext("foo/bar/ext"),
                ('foo/bar', '/ext'))
        self.assertEqual(riscospath.splitext("$.foo/bar/ext"),
                ('$.foo/bar', '/ext'))

    def test_splitdrive(self):
        tester('riscospath.splitdrive("SDFS::RISCOSPi.$")',
               ('SDFS::RISCOSPi.', '$'))
        tester('riscospath.splitdrive("Net#1.253:&.Spam")',
               ('Net#1.253:', '&.Spam'))
        tester('riscospath.splitdrive("Resources:$.Apps")',
               ('Resources:', '$.Apps'))
        tester('riscospath.splitdrive(":0.$.Work")',
               (':0.', '$.Work'))
        tester('riscospath.splitdrive(":0")',
               (':0', ''))
        tester('riscospath.splitdrive("SDFS::RISCOSPi")',
               ('SDFS::RISCOSPi', ''))
        tester('riscospath.splitdrive("$.Work")',
               ('', '$.Work'))
        tester('riscospath.splitdrive("ADFS:")',
               ('ADFS:', ''))

    def test_split(self):
        tester('riscospath.split("SDFS::RISCOSPi.$.foo.bar")', ('SDFS::RISCOSPi.$.foo', 'bar'))
        tester('riscospath.split("SDFS::RISCOSPi")', ('', 'SDFS::RISCOSPi'))
        tester('riscospath.split("SDFS::RISCOSPi.$")', ('SDFS::RISCOSPi', '$'))
        tester('riscospath.split("Eggs.Chips.Spam")', ('Eggs.Chips', 'Spam'))
        tester('riscospath.split("Eggs.Chips..")', ('Eggs.Chips', ''))
        tester('riscospath.split("")', ('', ''))

    def test_isabs(self):
        self.assertTrue (riscospath.isabs("SDFS::RISCOSPi.$"))
        self.assertTrue (riscospath.isabs("Net#.254:&.foo"))
        self.assertTrue (riscospath.isabs("SCSI:%"))
        self.assertFalse(riscospath.isabs("ADFS:baz"))
        self.assertFalse(riscospath.isabs("foo.bar"))

    def test_commonprefix(self):
        tester('riscospath.commonprefix(["$.swenson.spam", "$.swen.spam"])',
               "$.swen")
        tester('riscospath.commonprefix(["$.swen.spam", "$.swen.eggs"])',
               "$.swen.")
        tester('riscospath.commonprefix(["$.swen.spam", "$.swen.spam"])',
               "$.swen.spam")

    def test_join(self):
        tester('riscospath.join("")', '')
        tester('riscospath.join("", "", "")', '')
        tester('riscospath.join("a")', 'a')
        """
        tester('riscospath.join("/a")', '/a')
        tester('riscospath.join("\\a")', '\\a')
        tester('riscospath.join("a:")', 'a:')
        tester('riscospath.join("a:", "\\b")', 'a:\\b')
        tester('riscospath.join("a", "\\b")', '\\b')
        """
        tester('riscospath.join("a", "b", "c")', 'a.b.c')

        tester('riscospath.join("$.Work", "^")', '$.Work.^')
        tester('riscospath.join("$", "^")', '$.^')


        tester('riscospath.join("Net:&", "#0.254")', 'Net#0.254:&')
        tester('riscospath.join("Net#0.254:&", "$")', 'Net#0.254:$')
        tester('riscospath.join("Net#0.254:$", "ADFS:")', 'ADFS:')

        tester('riscospath.join("ADFS:", ":Spam")', 'ADFS::Spam')
        tester('riscospath.join("ADFS::0.$.Work", ":Spam")', 'ADFS::Spam')
        tester('riscospath.join("ADFS::0.$", ":Spam")', 'ADFS::Spam')
        tester('riscospath.join("ADFS:", ":Spam", "$")', 'ADFS::Spam.$')

        tester('riscospath.join("ADFS::Spam.$.Stuff", "&")', 'ADFS::Spam.&')
        tester('riscospath.join("ADFS::Spam.$.Stuff", "\\\\")', 'ADFS::Spam.\\')
        tester('riscospath.join("ADFS::Spam.$.Stuff", "%")', 'ADFS::Spam.%')
        tester('riscospath.join("ADFS::Spam.$.Stuff", "@")', 'ADFS::Spam.@')


        """
        tester('riscospath.join("a\\", "b", "c")', 'a\\b\\c')
        tester('riscospath.join("a", "b\\", "c")', 'a\\b\\c')
        tester('riscospath.join("a", "b", "\\c")', '\\c')
        tester('riscospath.join("d:\\", "\\pleep")', 'd:\\pleep')
        tester('riscospath.join("d:\\", "a", "b")', 'd:\\a\\b')

        tester("riscospath.join('', 'a')", 'a')
        tester("riscospath.join('', '', '', '', 'a')", 'a')
        tester("riscospath.join('a', '')", 'a\\')
        tester("riscospath.join('a', '', '', '', '')", 'a\\')
        tester("riscospath.join('a\\', '')", 'a\\')
        tester("riscospath.join('a\\', '', '', '', '')", 'a\\')
        tester("riscospath.join('a/', '')", 'a/')

        tester("riscospath.join('a/b', 'x/y')", 'a/b\\x/y')
        tester("riscospath.join('/a/b', 'x/y')", '/a/b\\x/y')
        tester("riscospath.join('/a/b/', 'x/y')", '/a/b/x/y')
        tester("riscospath.join('c:', 'x/y')", 'c:x/y')
        tester("riscospath.join('c:a/b', 'x/y')", 'c:a/b\\x/y')
        tester("riscospath.join('c:a/b/', 'x/y')", 'c:a/b/x/y')
        tester("riscospath.join('c:/', 'x/y')", 'c:/x/y')
        tester("riscospath.join('c:/a/b', 'x/y')", 'c:/a/b\\x/y')
        tester("riscospath.join('c:/a/b/', 'x/y')", 'c:/a/b/x/y')
        """
        tester("riscospath.join('a.b', '$.x.y')", '$.x.y')
        """
        tester("riscospath.join('c:', '/x/y')", 'c:/x/y')
        tester("riscospath.join('c:a/b', '/x/y')", 'c:/x/y')
        tester("riscospath.join('c:/', '/x/y')", 'c:/x/y')
        tester("riscospath.join('c:/a/b', '/x/y')", 'c:/x/y')

        tester("riscospath.join('c:', 'C:x/y')", 'C:x/y')
        tester("riscospath.join('c:a/b', 'C:x/y')", 'C:a/b\\x/y')
        tester("riscospath.join('c:/', 'C:x/y')", 'C:/x/y')
        tester("riscospath.join('c:/a/b', 'C:x/y')", 'C:/a/b\\x/y')
"""
        tester("riscospath.join('ADFS::HardDisc4.$.Work', 'Net:', ':Cheese', '#0.254:&', 'Spam', '$.Work')", 'Net#0.254::Cheese.$.Work')

    def test_normpath(self):
        tester("riscospath.normpath('A.B')", r'A.B')
        tester("riscospath.normpath('A..B')", r'A.B')
        tester("riscospath.normpath('A.foo.^.B')", r'A.B')
        tester("riscospath.normpath('$.^.B')", r'$.B')
        tester("riscospath.normpath('$.^')", r'$')
        tester("riscospath.normpath('SDFS::RISCOSPi.$')", r'SDFS::RISCOSPi.$')
        tester("riscospath.normpath('$.')", r'$')
        tester("riscospath.normpath('Net#0.254:Disc.&.Foo.^.')", r'Net#0.254:Disc.&')
        tester("riscospath.normpath('^.a.b')", r'^.a.b')

    """
    def test_realpath_pardir(self):
        expected = riscospath.normpath(os.getcwd())
        tester("riscospath.realpath('..')", riscospath.dirname(expected))
        tester("riscospath.realpath('../..')",
               riscospath.dirname(riscospath.dirname(expected)))
        tester("riscospath.realpath('/'.join(['..'] * 50))",
               riscospath.splitdrive(expected)[0] + '\\')
        tester("riscospath.realpath('..\\..')",
               riscospath.dirname(riscospath.dirname(expected)))
        tester("riscospath.realpath('\\'.join(['..'] * 50))",
               riscospath.splitdrive(expected)[0] + '\\')
    """

    """
    @unittest.skipUnless(HAVE_GETFINALPATHNAME, 'need _getfinalpathname')
    def test_realpath_nul(self):
        tester("riscospath.realpath('NUL')", r'\\.NUL')

    @unittest.skipUnless(HAVE_GETFINALPATHNAME, 'need _getfinalpathname')
    @unittest.skipUnless(HAVE_GETSHORTPATHNAME, 'need _getshortpathname')
    def test_realpath_cwd(self):
        ABSTFN = riscospath.abspath(support.TESTFN)

        support.unlink(ABSTFN)
        support.rmtree(ABSTFN)
        os.mkdir(ABSTFN)
        self.addCleanup(support.rmtree, ABSTFN)

        test_dir_long = riscospath.join(ABSTFN, "MyVeryLongDirectoryName")
        os.mkdir(test_dir_long)

        test_dir_short = _getshortpathname(test_dir_long)
        test_file_long = riscospath.join(test_dir_long, "file.txt")
        test_file_short = riscospath.join(test_dir_short, "file.txt")

        with open(test_file_long, "wb") as f:
            f.write(b"content")

        self.assertPathEqual(test_file_long, riscospath.realpath(test_file_short))

        with support.change_cwd(test_dir_long):
            self.assertPathEqual(test_file_long, riscospath.realpath("file.txt"))
        with support.change_cwd(test_dir_long.lower()):
            self.assertPathEqual(test_file_long, riscospath.realpath("file.txt"))
        with support.change_cwd(test_dir_short):
            self.assertPathEqual(test_file_long, riscospath.realpath("file.txt"))

    def test_expandvars(self):
        with support.EnvironmentVarGuard() as env:
            env.clear()
            env["foo"] = "bar"
            env["{foo"] = "baz1"
            env["{foo}"] = "baz2"
            tester('riscospath.expandvars("foo")', "foo")
            tester('riscospath.expandvars("$foo bar")', "bar bar")
            tester('riscospath.expandvars("${foo}bar")', "barbar")
            tester('riscospath.expandvars("$[foo]bar")', "$[foo]bar")
            tester('riscospath.expandvars("$bar bar")', "$bar bar")
            tester('riscospath.expandvars("$?bar")', "$?bar")
            tester('riscospath.expandvars("$foo}bar")', "bar}bar")
            tester('riscospath.expandvars("${foo")', "${foo")
            tester('riscospath.expandvars("${{foo}}")', "baz1}")
            tester('riscospath.expandvars("$foo$foo")', "barbar")
            tester('riscospath.expandvars("$bar$bar")', "$bar$bar")
            tester('riscospath.expandvars("%foo% bar")', "bar bar")
            tester('riscospath.expandvars("%foo%bar")', "barbar")
            tester('riscospath.expandvars("%foo%%foo%")', "barbar")
            tester('riscospath.expandvars("%%foo%%foo%foo%")', "%foo%foobar")
            tester('riscospath.expandvars("%?bar%")', "%?bar%")
            tester('riscospath.expandvars("%foo%%bar")', "bar%bar")
            tester('riscospath.expandvars("\'%foo%\'%bar")', "\'%foo%\'%bar")
            tester('riscospath.expandvars("bar\'%foo%")', "bar\'%foo%")

    @unittest.skipUnless(support.FS_NONASCII, 'need support.FS_NONASCII')
    def test_expandvars_nonascii(self):
        def check(value, expected):
            tester('riscospath.expandvars(%r)' % value, expected)
        with support.EnvironmentVarGuard() as env:
            env.clear()
            nonascii = support.FS_NONASCII
            env['spam'] = nonascii
            env[nonascii] = 'ham' + nonascii
            check('$spam bar', '%s bar' % nonascii)
            check('$%s bar' % nonascii, '$%s bar' % nonascii)
            check('${spam}bar', '%sbar' % nonascii)
            check('${%s}bar' % nonascii, 'ham%sbar' % nonascii)
            check('$spam}bar', '%s}bar' % nonascii)
            check('$%s}bar' % nonascii, '$%s}bar' % nonascii)
            check('%spam% bar', '%s bar' % nonascii)
            check('%{}% bar'.format(nonascii), 'ham%s bar' % nonascii)
            check('%spam%bar', '%sbar' % nonascii)
            check('%{}%bar'.format(nonascii), 'ham%sbar' % nonascii)

    def test_expanduser(self):
        tester('riscospath.expanduser("test")', 'test')

        with support.EnvironmentVarGuard() as env:
            env.clear()
            tester('riscospath.expanduser("~test")', '~test')

            env['HOMEPATH'] = 'eric\\idle'
            env['HOMEDRIVE'] = 'C:\\'
            tester('riscospath.expanduser("~test")', 'C:\\eric\\test')
            tester('riscospath.expanduser("~")', 'C:\\eric\\idle')

            del env['HOMEDRIVE']
            tester('riscospath.expanduser("~test")', 'eric\\test')
            tester('riscospath.expanduser("~")', 'eric\\idle')

            env.clear()
            env['USERPROFILE'] = 'C:\\eric\\idle'
            tester('riscospath.expanduser("~test")', 'C:\\eric\\test')
            tester('riscospath.expanduser("~")', 'C:\\eric\\idle')
            tester('riscospath.expanduser("~test\\foo\\bar")',
                   'C:\\eric\\test\\foo\\bar')
            tester('riscospath.expanduser("~test/foo/bar")',
                   'C:\\eric\\test/foo/bar')
            tester('riscospath.expanduser("~\\foo\\bar")',
                   'C:\\eric\\idle\\foo\\bar')
            tester('riscospath.expanduser("~/foo/bar")',
                   'C:\\eric\\idle/foo/bar')

            # bpo-36264: ignore `HOME` when set on windows
            env.clear()
            env['HOME'] = 'F:\\'
            env['USERPROFILE'] = 'C:\\eric\\idle'
            tester('riscospath.expanduser("~test")', 'C:\\eric\\test')
            tester('riscospath.expanduser("~")', 'C:\\eric\\idle')

    """
    def test_relpath(self):
        tester('riscospath.relpath("a")', 'a')
        tester('riscospath.relpath("a", "a")', '')
        tester('riscospath.relpath(riscospath.abspath("a"))', 'a')
        tester('riscospath.relpath("a.b")', 'a.b')
        tester('riscospath.relpath("^.a.b")', '^.a.b')
        with support.temp_cwd(support.TESTFN) as cwd_dir:
            currentdir = riscospath.basename(cwd_dir)
            tester('riscospath.relpath("a", "^.b")', '^.'+currentdir+'.a')
            tester('riscospath.relpath("a.b", "^.c")', '^.'+currentdir+'.a.b')
        tester('riscospath.relpath("a", "b.c")', '^.^.a')
        tester('riscospath.relpath("$.foo.bar.bat", "$.x.y")', '^.^.foo.bar.bat')
        tester('riscospath.relpath("SDFS::RISCOSPi.$.a", "SDFS::RISCOSPi.$.b.c")', '^.^.a')
        #tester('riscospath.relpath("/foo/bar/bat", "/x/y/z")', '..\\..\\..\\foo\\bar\\bat')
        #tester('riscospath.relpath("/foo/bar/bat", "/foo/bar")', 'bat')
        #tester('riscospath.relpath("/foo/bar/bat", "/")', 'foo\\bar\\bat')
        tester('riscospath.relpath("$", "$.foo.bar.bat")', '^.^.^')
        tester('riscospath.relpath("$", "$")', '')
        tester('riscospath.relpath("$.a", "$.a")', '')
        tester('riscospath.relpath("$.a.b", "$.a.b")', '')
        tester('riscospath.relpath("$.foo", "$.FOO")', '')
        self.assertRaises(ValueError,

            riscospath.relpath, "ADFS::0.$","SDFS::RISCOSPi.$.Work")
        self.assertRaises(ValueError,
            riscospath.relpath, "ADFS:$","ADFS:&")

    """
    def test_commonpath(self):
        def check(paths, expected):
            tester(('riscospath.commonpath(%r)' % paths).replace('\\\\', '\\'),
                   expected)
        def check_error(exc, paths):
            self.assertRaises(exc, riscospath.commonpath, paths)
            self.assertRaises(exc, riscospath.commonpath,
                              [os.fsencode(p) for p in paths])

        self.assertRaises(ValueError, riscospath.commonpath, [])
        check_error(ValueError, ['C:\\Program Files', 'Program Files'])
        check_error(ValueError, ['C:\\Program Files', 'C:Program Files'])
        check_error(ValueError, ['\\Program Files', 'Program Files'])
        check_error(ValueError, ['Program Files', 'C:\\Program Files'])
        check(['C:\\Program Files'], 'C:\\Program Files')
        check(['C:\\Program Files', 'C:\\Program Files'], 'C:\\Program Files')
        check(['C:\\Program Files\\', 'C:\\Program Files'],
              'C:\\Program Files')
        check(['C:\\Program Files\\', 'C:\\Program Files\\'],
              'C:\\Program Files')
        check(['C:\\\\Program Files', 'C:\\Program Files\\\\'],
              'C:\\Program Files')
        check(['C:\\.\\Program Files', 'C:\\Program Files\\.'],
              'C:\\Program Files')
        check(['C:\\', 'C:\\bin'], 'C:\\')
        check(['C:\\Program Files', 'C:\\bin'], 'C:\\')
        check(['C:\\Program Files', 'C:\\Program Files\\Bar'],
              'C:\\Program Files')
        check(['C:\\Program Files\\Foo', 'C:\\Program Files\\Bar'],
              'C:\\Program Files')
        check(['C:\\Program Files', 'C:\\Projects'], 'C:\\')
        check(['C:\\Program Files\\', 'C:\\Projects'], 'C:\\')

        check(['C:\\Program Files\\Foo', 'C:/Program Files/Bar'],
              'C:\\Program Files')
        check(['C:\\Program Files\\Foo', 'c:/program files/bar'],
              'C:\\Program Files')
        check(['c:/program files/bar', 'C:\\Program Files\\Foo'],
              'c:\\program files')

        check_error(ValueError, ['C:\\Program Files', 'D:\\Program Files'])

        check(['spam'], 'spam')
        check(['spam', 'spam'], 'spam')
        check(['spam', 'alot'], '')
        check(['and\\jam', 'and\\spam'], 'and')
        check(['and\\\\jam', 'and\\spam\\\\'], 'and')
        check(['and\\.\\jam', '.\\and\\spam'], 'and')
        check(['and\\jam', 'and\\spam', 'alot'], '')
        check(['and\\jam', 'and\\spam', 'and'], 'and')
        check(['C:and\\jam', 'C:and\\spam'], 'C:and')

        check([''], '')
        check(['', 'spam\\alot'], '')
        check_error(ValueError, ['', '\\spam\\alot'])

        self.assertRaises(TypeError, riscospath.commonpath,
                          [b'C:\\Program Files', 'C:\\Program Files\\Foo'])
        self.assertRaises(TypeError, riscospath.commonpath,
                          [b'C:\\Program Files', 'Program Files\\Foo'])
        self.assertRaises(TypeError, riscospath.commonpath,
                          [b'Program Files', 'C:\\Program Files\\Foo'])
        self.assertRaises(TypeError, riscospath.commonpath,
                          ['C:\\Program Files', b'C:\\Program Files\\Foo'])
        self.assertRaises(TypeError, riscospath.commonpath,
                          ['C:\\Program Files', b'Program Files\\Foo'])
        self.assertRaises(TypeError, riscospath.commonpath,
                          ['Program Files', b'C:\\Program Files\\Foo'])

    def test_sameopenfile(self):
        with TemporaryFile() as tf1, TemporaryFile() as tf2:
            # Make sure the same file is really the same
            self.assertTrue(riscospath.sameopenfile(tf1.fileno(), tf1.fileno()))
            # Make sure different files are really different
            self.assertFalse(riscospath.sameopenfile(tf1.fileno(), tf2.fileno()))
    """

    def test_ismount(self):
        self.assertTrue (riscospath.ismount("ADFS::HardDisc4.$"))
        self.assertTrue (riscospath.ismount("Net::Rachel.&"))
        self.assertTrue (riscospath.ismount("SDFS:%"))
        self.assertTrue (riscospath.ismount("SCSI:%"))
        self.assertTrue (riscospath.ismount("RAM:\\"))
        with support.temp_dir() as d:
            self.assertFalse(riscospath.ismount(d))

    """
    def assertEqualCI(self, s1, s2):
        "-"Assert that two strings are equal ignoring case differences."-"
        self.assertEqual(s1.lower(), s2.lower())



class PathLikeTests(NtpathTestCase):

    path = ntpath

    def setUp(self):
        self.file_name = support.TESTFN.lower()
        self.file_path = FakePath(support.TESTFN)
        self.addCleanup(support.unlink, self.file_name)
        with open(self.file_name, 'xb', 0) as file:
            file.write(b"test_riscospath.PathLikeTests")

    def _check_function(self, func):
        self.assertPathEqual(func(self.file_path), func(self.file_name))

    def test_path_normcase(self):
        self._check_function(self.path.normcase)

    def test_path_isabs(self):
        self._check_function(self.path.isabs)

    def test_path_join(self):
        self.assertEqual(self.path.join('a', FakePath('b'), 'c'),
                         self.path.join('a', 'b', 'c'))

    def test_path_split(self):
        self._check_function(self.path.split)

    def test_path_splitext(self):
        self._check_function(self.path.splitext)

    def test_path_splitdrive(self):
        self._check_function(self.path.splitdrive)

    def test_path_basename(self):
        self._check_function(self.path.basename)

    def test_path_dirname(self):
        self._check_function(self.path.dirname)

    def test_path_islink(self):
        self._check_function(self.path.islink)

    def test_path_lexists(self):
        self._check_function(self.path.lexists)

    def test_path_ismount(self):
        self._check_function(self.path.ismount)

    def test_path_expanduser(self):
        self._check_function(self.path.expanduser)

    def test_path_expandvars(self):
        self._check_function(self.path.expandvars)

    def test_path_normpath(self):
        self._check_function(self.path.normpath)

    def test_path_abspath(self):
        self._check_function(self.path.abspath)

    def test_path_realpath(self):
        self._check_function(self.path.realpath)

    def test_path_relpath(self):
        self._check_function(self.path.relpath)

    def test_path_commonpath(self):
        common_path = self.path.commonpath([self.file_path, self.file_name])
        self.assertPathEqual(common_path, self.file_name)

    def test_path_isdir(self):
        self._check_function(self.path.isdir)
"""

if __name__ == "__main__":
    unittest.main()
