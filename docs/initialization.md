%%% order 1

# Initialization
When declaring a variable, you may also initialize it with the set operator `:::asa =`.
For example:
```asa
x : int = 5;
```
The left hand side is the declaration `:::asa x : int`, and the right hand expression is the value it is initialized to, `5`.

If the expression on the right does not have a matching type, then the compiler will first search for any cast functions to perform an automatic cast.
```asa
x : int = 5.283;  // This casts the float `5.283` to an int32
```

-----

## Blank initialization
If there is no `:::asa =` set operator, then the variable will be zero-initialized. This means something different depending on what the datatype is, see [link](#zero-values).
```asa
y : int;
printl(y);
```
```
0
```

-----

## Zero values
In Asa, variables are initialized by default, even if not given a value. This is for safety, and to prevent unintentional memory leaks. But there are special values you may use to dictate this behavior on your own.

These are the regular "zero" values:
* `:::asa 0` for numbers
* `:::asa false` for booleans
* `:::asa ""` for strings
* `:::asa ''` for chars

And these are the special values:
### `:::asa default`
Resolves to the zero value of the required type automatically.
For example, `:::asa x : int = default;` would be `:::asa 0`.
But it also works for structs. For example:
```#asa
foo :: struct{
    x : int = 6;
    s : string = "hello!";
}

// Create instance
f : foo = default;

// Print the values that `default` initializes it to:
printl(f.x);  // 6
printl(f.s);  // "hello!"
```
Initializing to `:::asa default` gives the zero value for builtin types, or uses the default constructor for structs.

### `:::asa ?` or undefined
The symbol for undefined or unset data is `:::asa ?`. If you are heavily optimizing your software, or wish for a variable to not be initialized where it is declared for any reason, you can set it equal to this.
For example, leaving `:::asa x` undefined until much later in a program.
```asa
x : int = ?;

// ...

x = 4;
```
This value is also used as the zero-value of pointers specifically, and is the only case where `:::asa ?` and `:::asa default` have the same value:
```asa
a : *int = default;
b : *char = ?;

printl(int(a));  // 0
printl(int(b));  // 0
```

### As values
Both `:::asa ?` and `:::asa default` are also usable as values wherever literals are allowed, and the type is known. For example:
```asa
p : *int = ?;

if(p == ?)
    printl("Pointer is null");
```
Or:
```asa
s : string = "";

if(s == default)
    printl("String is empty");
```

