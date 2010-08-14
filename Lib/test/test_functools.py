import functools
import sys
import unittest
from test import support
from weakref import proxy
import pickle
from random import choice

@staticmethod
def PythonPartial(func, *args, **keywords):
    'Pure Python approximation of partial()'
    def newfunc(*fargs, **fkeywords):
        newkeywords = keywords.copy()
        newkeywords.update(fkeywords)
        return func(*(args + fargs), **newkeywords)
    newfunc.func = func
    newfunc.args = args
    newfunc.keywords = keywords
    return newfunc

def capture(*args, **kw):
    """capture all positional and keyword arguments"""
    return args, kw

def signature(part):
    """ return the signature of a partial object """
    return (part.func, part.args, part.keywords, part.__dict__)

class TestPartial(unittest.TestCase):

    thetype = functools.partial

    def test_basic_examples(self):
        p = self.thetype(capture, 1, 2, a=10, b=20)
        self.assertEqual(p(3, 4, b=30, c=40),
                         ((1, 2, 3, 4), dict(a=10, b=30, c=40)))
        p = self.thetype(map, lambda x: x*10)
        self.assertEqual(list(p([1,2,3,4])), [10, 20, 30, 40])

    def test_attributes(self):
        p = self.thetype(capture, 1, 2, a=10, b=20)
        # attributes should be readable
        self.assertEqual(p.func, capture)
        self.assertEqual(p.args, (1, 2))
        self.assertEqual(p.keywords, dict(a=10, b=20))
        # attributes should not be writable
        if not isinstance(self.thetype, type):
            return
        self.assertRaises(AttributeError, setattr, p, 'func', map)
        self.assertRaises(AttributeError, setattr, p, 'args', (1, 2))
        self.assertRaises(AttributeError, setattr, p, 'keywords', dict(a=1, b=2))

        p = self.thetype(hex)
        try:
            del p.__dict__
        except TypeError:
            pass
        else:
            self.fail('partial object allowed __dict__ to be deleted')

    def test_argument_checking(self):
        self.assertRaises(TypeError, self.thetype)     # need at least a func arg
        try:
            self.thetype(2)()
        except TypeError:
            pass
        else:
            self.fail('First arg not checked for callability')

    def test_protection_of_callers_dict_argument(self):
        # a caller's dictionary should not be altered by partial
        def func(a=10, b=20):
            return a
        d = {'a':3}
        p = self.thetype(func, a=5)
        self.assertEqual(p(**d), 3)
        self.assertEqual(d, {'a':3})
        p(b=7)
        self.assertEqual(d, {'a':3})

    def test_arg_combinations(self):
        # exercise special code paths for zero args in either partial
        # object or the caller
        p = self.thetype(capture)
        self.assertEqual(p(), ((), {}))
        self.assertEqual(p(1,2), ((1,2), {}))
        p = self.thetype(capture, 1, 2)
        self.assertEqual(p(), ((1,2), {}))
        self.assertEqual(p(3,4), ((1,2,3,4), {}))

    def test_kw_combinations(self):
        # exercise special code paths for no keyword args in
        # either the partial object or the caller
        p = self.thetype(capture)
        self.assertEqual(p(), ((), {}))
        self.assertEqual(p(a=1), ((), {'a':1}))
        p = self.thetype(capture, a=1)
        self.assertEqual(p(), ((), {'a':1}))
        self.assertEqual(p(b=2), ((), {'a':1, 'b':2}))
        # keyword args in the call override those in the partial object
        self.assertEqual(p(a=3, b=2), ((), {'a':3, 'b':2}))

    def test_positional(self):
        # make sure positional arguments are captured correctly
        for args in [(), (0,), (0,1), (0,1,2), (0,1,2,3)]:
            p = self.thetype(capture, *args)
            expected = args + ('x',)
            got, empty = p('x')
            self.assertTrue(expected == got and empty == {})

    def test_keyword(self):
        # make sure keyword arguments are captured correctly
        for a in ['a', 0, None, 3.5]:
            p = self.thetype(capture, a=a)
            expected = {'a':a,'x':None}
            empty, got = p(x=None)
            self.assertTrue(expected == got and empty == ())

    def test_no_side_effects(self):
        # make sure there are no side effects that affect subsequent calls
        p = self.thetype(capture, 0, a=1)
        args1, kw1 = p(1, b=2)
        self.assertTrue(args1 == (0,1) and kw1 == {'a':1,'b':2})
        args2, kw2 = p()
        self.assertTrue(args2 == (0,) and kw2 == {'a':1})

    def test_error_propagation(self):
        def f(x, y):
            x / y
        self.assertRaises(ZeroDivisionError, self.thetype(f, 1, 0))
        self.assertRaises(ZeroDivisionError, self.thetype(f, 1), 0)
        self.assertRaises(ZeroDivisionError, self.thetype(f), 1, 0)
        self.assertRaises(ZeroDivisionError, self.thetype(f, y=0), 1)

    def test_weakref(self):
        f = self.thetype(int, base=16)
        p = proxy(f)
        self.assertEqual(f.func, p.func)
        f = None
        self.assertRaises(ReferenceError, getattr, p, 'func')

    def test_with_bound_and_unbound_methods(self):
        data = list(map(str, range(10)))
        join = self.thetype(str.join, '')
        self.assertEqual(join(data), '0123456789')
        join = self.thetype(''.join)
        self.assertEqual(join(data), '0123456789')

    def test_pickle(self):
        f = self.thetype(signature, 'asdf', bar=True)
        f.add_something_to__dict__ = True
        f_copy = pickle.loads(pickle.dumps(f))
        self.assertEqual(signature(f), signature(f_copy))

class PartialSubclass(functools.partial):
    pass

class TestPartialSubclass(TestPartial):

    thetype = PartialSubclass

class TestPythonPartial(TestPartial):

    thetype = PythonPartial

    # the python version isn't picklable
    def test_pickle(self): pass

class TestUpdateWrapper(unittest.TestCase):

    def check_wrapper(self, wrapper, wrapped,
                      assigned=functools.WRAPPER_ASSIGNMENTS,
                      updated=functools.WRAPPER_UPDATES):
        # Check attributes were assigned
        for name in assigned:
            self.assertTrue(getattr(wrapper, name) is getattr(wrapped, name))
        # Check attributes were updated
        for name in updated:
            wrapper_attr = getattr(wrapper, name)
            wrapped_attr = getattr(wrapped, name)
            for key in wrapped_attr:
                self.assertTrue(wrapped_attr[key] is wrapper_attr[key])

    def _default_update(self):
        def f(a:'This is a new annotation'):
            """This is a test"""
            pass
        f.attr = 'This is also a test'
        def wrapper(b:'This is the prior annotation'):
            pass
        functools.update_wrapper(wrapper, f)
        return wrapper, f

    def test_default_update(self):
        wrapper, f = self._default_update()
        self.check_wrapper(wrapper, f)
        self.assertEqual(wrapper.__name__, 'f')
        self.assertEqual(wrapper.attr, 'This is also a test')
        self.assertEqual(wrapper.__annotations__['a'], 'This is a new annotation')
        self.assertNotIn('b', wrapper.__annotations__)

    @unittest.skipIf(sys.flags.optimize >= 2,
                     "Docstrings are omitted with -O2 and above")
    def test_default_update_doc(self):
        wrapper, f = self._default_update()
        self.assertEqual(wrapper.__doc__, 'This is a test')

    def test_no_update(self):
        def f():
            """This is a test"""
            pass
        f.attr = 'This is also a test'
        def wrapper():
            pass
        functools.update_wrapper(wrapper, f, (), ())
        self.check_wrapper(wrapper, f, (), ())
        self.assertEqual(wrapper.__name__, 'wrapper')
        self.assertEqual(wrapper.__doc__, None)
        self.assertEqual(wrapper.__annotations__, {})
        self.assertFalse(hasattr(wrapper, 'attr'))

    def test_selective_update(self):
        def f():
            pass
        f.attr = 'This is a different test'
        f.dict_attr = dict(a=1, b=2, c=3)
        def wrapper():
            pass
        wrapper.dict_attr = {}
        assign = ('attr',)
        update = ('dict_attr',)
        functools.update_wrapper(wrapper, f, assign, update)
        self.check_wrapper(wrapper, f, assign, update)
        self.assertEqual(wrapper.__name__, 'wrapper')
        self.assertEqual(wrapper.__doc__, None)
        self.assertEqual(wrapper.attr, 'This is a different test')
        self.assertEqual(wrapper.dict_attr, f.dict_attr)

    def test_builtin_update(self):
        # Test for bug #1576241
        def wrapper():
            pass
        functools.update_wrapper(wrapper, max)
        self.assertEqual(wrapper.__name__, 'max')
        self.assertTrue(wrapper.__doc__.startswith('max('))
        self.assertEqual(wrapper.__annotations__, {})

class TestWraps(TestUpdateWrapper):

    def _default_update(self):
        def f():
            """This is a test"""
            pass
        f.attr = 'This is also a test'
        @functools.wraps(f)
        def wrapper():
            pass
        self.check_wrapper(wrapper, f)
        return wrapper

    def test_default_update(self):
        wrapper = self._default_update()
        self.assertEqual(wrapper.__name__, 'f')
        self.assertEqual(wrapper.attr, 'This is also a test')

    @unittest.skipIf(not sys.flags.optimize <= 1,
                     "Docstrings are omitted with -O2 and above")
    def test_default_update_doc(self):
        wrapper = self._default_update()
        self.assertEqual(wrapper.__doc__, 'This is a test')

    def test_no_update(self):
        def f():
            """This is a test"""
            pass
        f.attr = 'This is also a test'
        @functools.wraps(f, (), ())
        def wrapper():
            pass
        self.check_wrapper(wrapper, f, (), ())
        self.assertEqual(wrapper.__name__, 'wrapper')
        self.assertEqual(wrapper.__doc__, None)
        self.assertFalse(hasattr(wrapper, 'attr'))

    def test_selective_update(self):
        def f():
            pass
        f.attr = 'This is a different test'
        f.dict_attr = dict(a=1, b=2, c=3)
        def add_dict_attr(f):
            f.dict_attr = {}
            return f
        assign = ('attr',)
        update = ('dict_attr',)
        @functools.wraps(f, assign, update)
        @add_dict_attr
        def wrapper():
            pass
        self.check_wrapper(wrapper, f, assign, update)
        self.assertEqual(wrapper.__name__, 'wrapper')
        self.assertEqual(wrapper.__doc__, None)
        self.assertEqual(wrapper.attr, 'This is a different test')
        self.assertEqual(wrapper.dict_attr, f.dict_attr)

class TestReduce(unittest.TestCase):
    func = functools.reduce

    def test_reduce(self):
        class Squares:
            def __init__(self, max):
                self.max = max
                self.sofar = []

            def __len__(self):
                return len(self.sofar)

            def __getitem__(self, i):
                if not 0 <= i < self.max: raise IndexError
                n = len(self.sofar)
                while n <= i:
                    self.sofar.append(n*n)
                    n += 1
                return self.sofar[i]

        self.assertEqual(self.func(lambda x, y: x+y, ['a', 'b', 'c'], ''), 'abc')
        self.assertEqual(
            self.func(lambda x, y: x+y, [['a', 'c'], [], ['d', 'w']], []),
            ['a','c','d','w']
        )
        self.assertEqual(self.func(lambda x, y: x*y, range(2,8), 1), 5040)
        self.assertEqual(
            self.func(lambda x, y: x*y, range(2,21), 1),
            2432902008176640000
        )
        self.assertEqual(self.func(lambda x, y: x+y, Squares(10)), 285)
        self.assertEqual(self.func(lambda x, y: x+y, Squares(10), 0), 285)
        self.assertEqual(self.func(lambda x, y: x+y, Squares(0), 0), 0)
        self.assertRaises(TypeError, self.func)
        self.assertRaises(TypeError, self.func, 42, 42)
        self.assertRaises(TypeError, self.func, 42, 42, 42)
        self.assertEqual(self.func(42, "1"), "1") # func is never called with one item
        self.assertEqual(self.func(42, "", "1"), "1") # func is never called with one item
        self.assertRaises(TypeError, self.func, 42, (42, 42))

        class BadSeq:
            def __getitem__(self, index):
                raise ValueError
        self.assertRaises(ValueError, self.func, 42, BadSeq())

    # Test reduce()'s use of iterators.
    def test_iterator_usage(self):
        class SequenceClass:
            def __init__(self, n):
                self.n = n
            def __getitem__(self, i):
                if 0 <= i < self.n:
                    return i
                else:
                    raise IndexError

        from operator import add
        self.assertEqual(self.func(add, SequenceClass(5)), 10)
        self.assertEqual(self.func(add, SequenceClass(5), 42), 52)
        self.assertRaises(TypeError, self.func, add, SequenceClass(0))
        self.assertEqual(self.func(add, SequenceClass(0), 42), 42)
        self.assertEqual(self.func(add, SequenceClass(1)), 0)
        self.assertEqual(self.func(add, SequenceClass(1), 42), 42)

        d = {"one": 1, "two": 2, "three": 3}
        self.assertEqual(self.func(add, d), "".join(d.keys()))

class TestCmpToKey(unittest.TestCase):
    def test_cmp_to_key(self):
        def mycmp(x, y):
            return y - x
        self.assertEqual(sorted(range(5), key=functools.cmp_to_key(mycmp)),
                         [4, 3, 2, 1, 0])

    def test_hash(self):
        def mycmp(x, y):
            return y - x
        key = functools.cmp_to_key(mycmp)
        k = key(10)
        self.assertRaises(TypeError, hash(k))

class TestTotalOrdering(unittest.TestCase):

    def test_total_ordering_lt(self):
        @functools.total_ordering
        class A:
            def __init__(self, value):
                self.value = value
            def __lt__(self, other):
                return self.value < other.value
        self.assert_(A(1) < A(2))
        self.assert_(A(2) > A(1))
        self.assert_(A(1) <= A(2))
        self.assert_(A(2) >= A(1))
        self.assert_(A(2) <= A(2))
        self.assert_(A(2) >= A(2))

    def test_total_ordering_le(self):
        @functools.total_ordering
        class A:
            def __init__(self, value):
                self.value = value
            def __le__(self, other):
                return self.value <= other.value
        self.assert_(A(1) < A(2))
        self.assert_(A(2) > A(1))
        self.assert_(A(1) <= A(2))
        self.assert_(A(2) >= A(1))
        self.assert_(A(2) <= A(2))
        self.assert_(A(2) >= A(2))

    def test_total_ordering_gt(self):
        @functools.total_ordering
        class A:
            def __init__(self, value):
                self.value = value
            def __gt__(self, other):
                return self.value > other.value
        self.assert_(A(1) < A(2))
        self.assert_(A(2) > A(1))
        self.assert_(A(1) <= A(2))
        self.assert_(A(2) >= A(1))
        self.assert_(A(2) <= A(2))
        self.assert_(A(2) >= A(2))

    def test_total_ordering_ge(self):
        @functools.total_ordering
        class A:
            def __init__(self, value):
                self.value = value
            def __ge__(self, other):
                return self.value >= other.value
        self.assert_(A(1) < A(2))
        self.assert_(A(2) > A(1))
        self.assert_(A(1) <= A(2))
        self.assert_(A(2) >= A(1))
        self.assert_(A(2) <= A(2))
        self.assert_(A(2) >= A(2))

    def test_total_ordering_no_overwrite(self):
        # new methods should not overwrite existing
        @functools.total_ordering
        class A(int):
            raise Exception()
        self.assert_(A(1) < A(2))
        self.assert_(A(2) > A(1))
        self.assert_(A(1) <= A(2))
        self.assert_(A(2) >= A(1))
        self.assert_(A(2) <= A(2))
        self.assert_(A(2) >= A(2))

    def test_no_operations_defined(self):
        with self.assertRaises(ValueError):
            @functools.total_ordering
            class A:
                pass

class TestLRU(unittest.TestCase):

    def test_lru(self):
        def orig(x, y):
            return 3*x+y
        f = functools.lru_cache(maxsize=20)(orig)

        domain = range(5)
        for i in range(1000):
            x, y = choice(domain), choice(domain)
            actual = f(x, y)
            expected = orig(x, y)
            self.assertEquals(actual, expected)
        self.assert_(f.hits > f.misses)
        self.assertEquals(f.hits + f.misses, 1000)

        f.clear()   # test clearing
        self.assertEqual(f.hits, 0)
        self.assertEqual(f.misses, 0)
        f(x, y)
        self.assertEqual(f.hits, 0)
        self.assertEqual(f.misses, 1)

        # test size zero (which means "never-cache")
        f_cnt = 0
        @functools.lru_cache(0)
        def f():
            nonlocal f_cnt
            f_cnt += 1
            return 20
        self.assertEqual(f(), 20)
        self.assertEqual(f(), 20)
        self.assertEqual(f(), 20)
        self.assertEqual(f_cnt, 3)

        # test size one
        f_cnt = 0
        @functools.lru_cache(1)
        def f():
            nonlocal f_cnt
            f_cnt += 1
            return 20
        self.assertEqual(f(), 20)
        self.assertEqual(f(), 20)
        self.assertEqual(f(), 20)
        self.assertEqual(f_cnt, 1)

    def test_lfu(self):
        def orig(x, y):
            return 3*x+y
        f = functools.lfu_cache(maxsize=20)(orig)

        domain = range(5)
        for i in range(1000):
            x, y = choice(domain), choice(domain)
            actual = f(x, y)
            expected = orig(x, y)
            self.assertEquals(actual, expected)
        self.assert_(f.hits > f.misses)
        self.assertEquals(f.hits + f.misses, 1000)

        f.clear()   # test clearing
        self.assertEqual(f.hits, 0)
        self.assertEqual(f.misses, 0)
        f(x, y)
        self.assertEqual(f.hits, 0)
        self.assertEqual(f.misses, 1)

        # test size zero (which means "never-cache")
        f_cnt = 0
        @functools.lfu_cache(0)
        def f():
            nonlocal f_cnt
            f_cnt += 1
            return 20
        self.assertEqual(f(), 20)
        self.assertEqual(f(), 20)
        self.assertEqual(f(), 20)
        self.assertEqual(f_cnt, 3)

        # test size one
        f_cnt = 0
        @functools.lfu_cache(1)
        def f():
            nonlocal f_cnt
            f_cnt += 1
            return 20
        self.assertEqual(f(), 20)
        self.assertEqual(f(), 20)
        self.assertEqual(f(), 20)
        self.assertEqual(f_cnt, 1)

def test_main(verbose=None):
    test_classes = (
        TestPartial,
        TestPartialSubclass,
        TestPythonPartial,
        TestUpdateWrapper,
        TestWraps,
        TestReduce,
        TestLRU,
    )
    support.run_unittest(*test_classes)

    # verify reference counting
    if verbose and hasattr(sys, "gettotalrefcount"):
        import gc
        counts = [None] * 5
        for i in range(len(counts)):
            support.run_unittest(*test_classes)
            gc.collect()
            counts[i] = sys.gettotalrefcount()
        print(counts)

if __name__ == '__main__':
    test_main(verbose=True)
