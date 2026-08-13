# --- user-defined operators (Ch6) ---
def unary!(v) if v then 0 else 1;
def unary-(v) 0-v;
def binary > 10 (LHS RHS) RHS < LHS;
def binary | 5 (LHS RHS) if LHS then 1 else if RHS then 1 else 0;
def binary : 1 (x y) y;

# --- control flow (Ch5) ---
def nested(x) if x < 0 then -1 else if x < 10 then 0 else 1;
def sumstep(n) var s = 0 in (for i = 0, i < n, 2 in s = s + i) : s;

!0;
-(5);
7 > 3;
0 | 3;
nested(-4); nested(5); nested(50);
sumstep(10);
(1 < 2) | (5 > 9);
