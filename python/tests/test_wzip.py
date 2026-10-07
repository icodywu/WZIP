"""Tests of the Python package: python -m unittest discover -s python/tests (after pip install .)"""
import os
import random
import tempfile
import threading
import unittest

import wzip

GOLDEN = os.path.join(os.path.dirname(__file__), "..", "..", "tests", "golden")


def sample(n, seed=1):
    r = random.Random(seed)
    words = [b"the ", b"window ", b"length ", b"offset ", b"of ", b"LZ77 ", b"code ", b"a "]
    out = bytearray()
    while len(out) < n:
        out += r.choice(words) if r.random() < 0.95 else bytes([r.randrange(256)])
    return bytes(out[:n])


class RoundTrip(unittest.TestCase):
    def test_frames(self):
        for n in (0, 1, 31, 5000, 70000, 300000):
            data = sample(n, n)
            for codec, levels in (("wzip", (0, 1, 5, 9)), ("wlz4", ("fast", "lazy", 0, 2, 10))):
                for level in levels:
                    for block_log in (0, 12):
                        packed = wzip.compress(data, codec, level, block_log)
                        self.assertEqual(wzip.decompress(packed), data)
                        self.assertEqual(wzip.content_size(packed), n)

    def test_bare_streams(self):
        data = sample(100000)
        self.assertEqual(wzip.wzip_decompress(wzip.wzip_compress(data, 5)), data)
        self.assertEqual(wzip.wlz4_decompress(wzip.wlz4_compress(data, 10)), data)
        self.assertEqual(wzip.wlz4_decompress(wzip.wlz4_compress(b"")), b"")
        self.assertEqual(wzip.wzip_decompress(wzip.wzip_compress(b"")), b"")

    def test_buffers(self):
        data = sample(20000)
        self.assertEqual(wzip.decompress(wzip.compress(bytearray(data))), data)
        self.assertEqual(wzip.decompress(memoryview(wzip.compress(memoryview(data)))), data)

    def test_threads_same_frame(self):
        data = sample(400000, 7)
        for codec, level, block_log in (("wzip", 5, 15), ("wzip", 11, 0), ("wlz4", 10, 15)):
            one = wzip.compress(data, codec, level, block_log)
            self.assertEqual(wzip.compress(data, codec, level, block_log, threads=6), one)
            self.assertEqual(wzip.decompress(one), data)

    def test_concatenated_frames(self):
        a, b = sample(9000, 2), sample(40000, 3)
        self.assertEqual(wzip.decompress(wzip.compress(a) + wzip.compress(b, "wlz4")), a + b)

    def test_threads(self):
        data = [sample(200000, k) for k in range(4)]
        results = [None] * 8

        def work(i):
            results[i] = wzip.decompress(wzip.compress(data[i % 4], "wzip" if i & 1 else "wlz4", 5 if i & 1 else 10))

        threads = [threading.Thread(target=work, args=(i,)) for i in range(8)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        for i in range(8):
            self.assertEqual(results[i], data[i % 4])

    def test_files(self):
        data = sample(50000)
        with tempfile.TemporaryDirectory() as d:
            name = os.path.join(d, "x.wz")
            with wzip.open(name, "wb", "wlz4") as f:
                f.write(data)
            self.assertEqual(wzip.open(name), data)


class Errors(unittest.TestCase):
    def test_damaged(self):
        packed = bytearray(wzip.compress(sample(30000)))
        with self.assertRaises(wzip.Error):
            wzip.decompress(bytes(packed[:-1]))
        packed[len(packed) // 2] ^= 0x40
        with self.assertRaises(wzip.Error):
            wzip.decompress(bytes(packed))
        with self.assertRaises(wzip.Error):
            wzip.decompress(b"not a frame at all")

    def test_arguments(self):
        with self.assertRaises(ValueError):
            wzip.compress(b"x", "zstd")
        with self.assertRaises(ValueError):
            wzip.compress(b"x", "wzip", 14)
        with self.assertRaises(ValueError):
            wzip.compress(b"x", "wlz4", "slow")


@unittest.skipUnless(os.path.isdir(GOLDEN), "golden frames not found")
class Golden(unittest.TestCase):
    def test_golden(self):
        names = [n for n in os.listdir(GOLDEN) if n.endswith((".wz", ".wlz4"))]
        self.assertTrue(names)
        for name in names:
            ref = "concat.ref" if name.startswith("concat.") else name.split(".L")[0].split(".lazy")[0].split(".fast")[0]
            with open(os.path.join(GOLDEN, name), "rb") as f, open(os.path.join(GOLDEN, ref), "rb") as g:
                self.assertEqual(wzip.decompress(f.read()), g.read(), name)


if __name__ == "__main__":
    unittest.main()
