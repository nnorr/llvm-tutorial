def binary : 1 (x y) y;
def fib(n)
  var a = 0, b = 1, c in
  (for i = 1, i < n in
     (c = a + b : a = b : b = c)) : b;
fib(10);
