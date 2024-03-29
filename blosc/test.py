from __future__ import division
import sys
import gc
import os
from distutils.version import LooseVersion
import ctypes
import blosc

# version number hack
vi = sys.version_info
PY26 = vi[0] == 2 and vi[1] == 6
PY27 = vi[0] == 2 and vi[1] == 7
PY3X = vi[0] == 3

if PY26:
    import unittest2 as unittest
else:
    import unittest

try:
    import numpy
except ImportError:
    has_numpy = False
else:
    has_numpy = True

try:
    import psutil
except ImportError:
    psutil = None


class TestCodec(unittest.TestCase):

    def test_basic_codec(self):
        s = b'0123456789'
        c = blosc.compress(s, typesize=1)
        d = blosc.decompress(c)
        self.assertEqual(s, d)

    def test_get_clib(self):
        s = b'0123456789'
        for cname in blosc.compressor_list():
            c = blosc.compress(s, typesize=1, cname=cname)
            clib = blosc.get_clib(c)
            self.assert_(clib == blosc.cname2clib[cname])

    def test_all_compressors(self):
        s = b'0123456789' * 100
        for cname in blosc.compressor_list():
            c = blosc.compress(s, typesize=1, cname=cname)
            d = blosc.decompress(c)
            self.assertEqual(s, d)

    def test_all_filters(self):
        s = b'0123456789' * 100
        filters = [blosc.NOSHUFFLE, blosc.SHUFFLE]
        # BITFILTER only works properly from 1.8.0 on
        if LooseVersion(blosc.blosclib_version) >= LooseVersion("1.8.0"):
            filters.append(blosc.BITSHUFFLE)
        for filter_ in filters:
            c = blosc.compress(s, typesize=1, shuffle=filter_)
            d = blosc.decompress(c)
            self.assertEqual(s, d)

    def test_compress_input_types(self):
        import numpy as np
        # assume the expected answer was compressed from bytes
        expected = blosc.compress(b'0123456789', typesize=1)

        if not PY3X:
            # Python 3 can't compress unicode
            self.assertEqual(expected,
                             blosc.compress(u'0123456789', typesize=1))
            # And the basic string is unicode
            self.assertEqual(expected,
                             blosc.compress('0123456789', typesize=1))

        # now for all the things that support the buffer interface
        if not PY3X:
            # Python 3 no longer has the buffer
            self.assertEqual(expected, blosc.compress(
                buffer(b'0123456789'), typesize=1))
        if not PY26:
            # memoryview doesn't exist on Python 2.6
            self.assertEqual(expected, blosc.compress(
                memoryview(b'0123456789'), typesize=1))

        self.assertEqual(expected, blosc.compress(
            bytearray(b'0123456789'), typesize=1))
        self.assertEqual(expected, blosc.compress(
            np.array([b'0123456789']), typesize=1))

    def test_decompress_input_types(self):
        import numpy as np
        # assume the expected answer was compressed from bytes
        expected = b'0123456789'
        compressed = blosc.compress(expected, typesize=1)

        # now for all the things that support the buffer interface
        if not PY3X:
            # Python 3 no longer has the buffer
            self.assertEqual(expected, blosc.decompress(buffer(compressed)))
        if not PY26:
            # memoryview doesn't exist on Python 2.6
            self.assertEqual(expected,
                             blosc.decompress(memoryview(compressed)))

        self.assertEqual(expected, blosc.decompress(bytearray(compressed)))
        self.assertEqual(expected, blosc.decompress(np.array([compressed])))

    def test_decompress_releasegil(self):
        import numpy as np
        # assume the expected answer was compressed from bytes
        blosc.set_releasegil(True)
        expected = b'0123456789'
        compressed = blosc.compress(expected, typesize=1)

        # now for all the things that support the buffer interface
        if not PY3X:
            # Python 3 no longer has the buffer
            self.assertEqual(expected, blosc.decompress(buffer(compressed)))
        if not PY26:
            # memoryview doesn't exist on Python 2.6
            self.assertEqual(expected,
                             blosc.decompress(memoryview(compressed)))

        self.assertEqual(expected, blosc.decompress(bytearray(compressed)))
        self.assertEqual(expected, blosc.decompress(np.array([compressed])))
        blosc.set_releasegil(False)

    def test_decompress_input_types_as_bytearray(self):
        import numpy as np
        # assume the expected answer was compressed from bytes
        expected = bytearray(b'0123456789')
        compressed = blosc.compress(expected, typesize=1)

        # now for all the things that support the buffer interface
        if not PY3X:
            # Python 3 no longer has the buffer
            self.assertEqual(expected, blosc.decompress(buffer(compressed),
                                                        as_bytearray=True))
        if not PY26:
            # memoryview doesn't exist on Python 2.6
            self.assertEqual(expected,
                             blosc.decompress(memoryview(compressed),
                                              as_bytearray=True))

        self.assertEqual(expected, blosc.decompress(bytearray(compressed),
                                                    as_bytearray=True))
        self.assertEqual(expected, blosc.decompress(np.array([compressed]),
                                                    as_bytearray=True))

    def test_compress_exceptions(self):
        s = b'0123456789'

        self.assertRaises(ValueError, blosc.compress, s, typesize=0)
        self.assertRaises(ValueError, blosc.compress, s,
                          typesize=blosc.MAX_TYPESIZE + 1)

        self.assertRaises(ValueError, blosc.compress, s, typesize=1, clevel=-1)
        self.assertRaises(ValueError, blosc.compress, s, typesize=1, clevel=10)

        self.assertRaises(TypeError, blosc.compress, 1.0, 1)
        self.assertRaises(TypeError, blosc.compress, ['abc'], 1)

        self.assertRaises(ValueError, blosc.compress, 'abc',
                          typesize=1, cname='foo')

        if PY3X:
            # Python 3 doesn't support unicode
            self.assertRaises(ValueError, blosc.compress,
                              '0123456789', typesize=0)

        # Create a simple mock to avoid having to create a buffer of 2 GB
        class LenMock(object):
            def __len__(self):
                return blosc.MAX_BUFFERSIZE + 1
        self.assertRaises(ValueError, blosc.compress, LenMock(), typesize=1)

    def test_compress_ptr_exceptions(self):
        # Make sure we do have a valid address, to reduce the chance of a
        # segfault if we do actually start compressing because the exceptions
        # aren't raised.
        typesize, items = 8, 8
        data = [float(i) for i in range(items)]
        Array = ctypes.c_double * items
        array = Array(*data)
        address = ctypes.addressof(array)

        self.assertRaises(ValueError, blosc.compress_ptr, address, items,
                          typesize=-1)
        self.assertRaises(ValueError, blosc.compress_ptr, address, items,
                          typesize=blosc.MAX_TYPESIZE + 1)

        self.assertRaises(ValueError, blosc.compress_ptr, address, items,
                          typesize=typesize, clevel=-1)
        self.assertRaises(ValueError, blosc.compress_ptr, address, items,
                          typesize=typesize, clevel=10)

        self.assertRaises(TypeError, blosc.compress_ptr, 1.0, items,
                          typesize=typesize)
        self.assertRaises(TypeError, blosc.compress_ptr, ['abc'], items,
                          typesize=typesize)

        self.assertRaises(ValueError, blosc.compress_ptr, address, -1,
                          typesize=typesize)
        self.assertRaises(ValueError, blosc.compress_ptr, address,
                          blosc.MAX_BUFFERSIZE + 1, typesize=typesize)

    def test_decompress_exceptions(self):
        self.assertRaises(TypeError, blosc.decompress, 1.0)
        self.assertRaises(TypeError, blosc.decompress, ['abc'])

    def test_decompress_ptr_exceptions(self):
        # make sure we do have a valid address
        typesize, items = 8, 8
        data = [float(i) for i in range(items)]
        Array = ctypes.c_double * items
        in_array = Array(*data)
        c = blosc.compress_ptr(ctypes.addressof(in_array), items, typesize)
        out_array = ctypes.create_string_buffer(items * typesize)

        self.assertRaises(TypeError, blosc.decompress_ptr, 1.0,
                          ctypes.addressof(out_array))
        self.assertRaises(TypeError, blosc.decompress_ptr, ['abc'],
                          ctypes.addressof(out_array))

        self.assertRaises(TypeError, blosc.decompress_ptr, c, 1.0)
        self.assertRaises(TypeError, blosc.decompress_ptr, c, ['abc'])

    @unittest.skipIf(not has_numpy, "Numpy not available")
    def test_pack_array_exceptions(self):

        self.assertRaises(TypeError, blosc.pack_array, 'abc')
        self.assertRaises(TypeError, blosc.pack_array, 1.0)

        items = (blosc.MAX_BUFFERSIZE // 8) + 1
        one = numpy.ones(1, dtype=numpy.int64)
        self.assertRaises(ValueError, blosc.pack_array, one, clevel=-1)
        self.assertRaises(ValueError, blosc.pack_array, one, clevel=10)

        # use stride trick to make an array that looks like a huge one
        ones = numpy.lib.stride_tricks.as_strided(one, shape=(1, items),
                                                  strides=(8, 0))[0]

        # This should always raise an error
        self.assertRaises(ValueError, blosc.pack_array, ones)

    def test_unpack_array_exceptions(self):
        self.assertRaises(TypeError, blosc.unpack_array, 1.0)

    @unittest.skipIf(not psutil, "psutil not available, cannot test for leaks")
    def test_no_leaks(self):

        num_elements = 10000000
        typesize = 8
        data = [float(i) for i in range(num_elements)]  # ~76MB
        Array = ctypes.c_double * num_elements
        array = Array(*data)
        address = ctypes.addressof(array)

        def leaks(operation, repeats=3):
            gc.collect()
            used_mem_before = psutil.Process(os.getpid()).memory_info()[0]
            for _ in range(repeats):
                operation()
            gc.collect()
            used_mem_after = psutil.Process(os.getpid()).memory_info()[0]
            # We multiply by an additional factor of .01 to account for
            # storage overhead of Python classes
            return (used_mem_after - used_mem_before) >= num_elements * 8.01

        def compress():
            blosc.compress(array, typesize, clevel=1)

        def compress_ptr():
            blosc.compress_ptr(address, num_elements, typesize, clevel=0)

        def decompress():
            cx = blosc.compress(array, typesize, clevel=1)
            blosc.decompress(cx)

        def decompress_ptr():
            cx = blosc.compress_ptr(address, num_elements, typesize, clevel=0)
            blosc.decompress_ptr(cx, address)

        self.assertFalse(leaks(compress), msg='compress leaks memory')
        self.assertFalse(leaks(compress_ptr), msg='compress_ptr leaks memory')
        self.assertFalse(leaks(decompress), msg='decompress leaks memory')
        self.assertFalse(leaks(decompress_ptr),
                         msg='decompress_ptr leaks memory')

    def test_get_blocksize(self):
        s = b'0123456789' * 1000
        blosc.set_blocksize(2**14)
        blosc.compress(s, typesize=1)
        d = blosc.get_blocksize()
        self.assertEqual(d, 2**14)

    def test_get_cbuffer_sizes(self):
        s = b'0123456789' * 100000
        blosc.set_blocksize(2**16)
        c = blosc.compress(s, typesize=1)
        t = blosc.get_cbuffer_sizes(c)
        self.assertEqual(t, (1000000, 4354, 2**16))


def run(verbosity=2):
    import blosc
    import blosc.toplevel
    blosc.print_versions()
    suite = unittest.TestLoader().loadTestsFromTestCase(TestCodec)
    # If in the future we split this test file in several, the auto-discover
    # might be interesting

    # suite = unittest.TestLoader().discover(start_dir='.', pattern='test*.py')
    suite.addTests(unittest.TestLoader().loadTestsFromModule(blosc.toplevel))
    assert unittest.TextTestRunner(verbosity=verbosity).\
        run(suite).wasSuccessful()


if __name__ == '__main__':
    run()
