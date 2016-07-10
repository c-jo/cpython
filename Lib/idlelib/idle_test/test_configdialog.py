'''Test idlelib.configdialog.

Coverage: 46% just by creating dialog.
The other half is code for working with user customizations.
'''
from idlelib.configdialog import ConfigDialog  # always test import
from test.support import requires
requires('gui')
from tkinter import Tk
import unittest

class ConfigDialogTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.root = Tk()

    @classmethod
    def tearDownClass(cls):
        cls.root.update_idletasks()
        cls.root.destroy()
        del cls.root

    def test_configdialog(self):
        d = ConfigDialog(self.root, 'Test', _utest=True)
        d.remove_var_callbacks()


if __name__ == '__main__':
    unittest.main(verbosity=2)
