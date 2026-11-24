"""Run with the original framework Python to exercise its shipped _ctypes."""
import ctypes as c
import sys

libc = c.CDLL(None)
libc.strlen.argtypes = [c.c_char_p]
libc.strlen.restype = c.c_size_t
assert libc.strlen(b"abcdefgh") == 8

# qsort enters the Python callback through native libc, which also checks
# the argument vector layout and a negative signed scalar return.
compare_type = c.CFUNCTYPE(c.c_int, c.POINTER(c.c_int), c.POINTER(c.c_int))
calls = []

@compare_type
def compare(left, right):
    a, b = left[0], right[0]
    calls.append((a, b))
    return (a > b) - (a < b)

libc.qsort.argtypes = [c.c_void_p, c.c_size_t, c.c_size_t, compare_type]
libc.qsort.restype = None
values = (c.c_int * 5)(3, -1, 2, 3, 0)
libc.qsort(values, len(values), c.sizeof(c.c_int), compare)
assert list(values) == [-1, 0, 2, 3, 3] and calls
print("ctypes native call and callback passed", sys.version.split()[0])
