%%% order 1

# Initialization
When declaring a variable, you may also initialize it with a value, using the set operator `:::asa =`.
For example:
```!asa
x : int = 5;
```
The left hand side is the declaration `:::asa x : int`, and the right hand expression is the value it is initialized to, `:::asa 5`.


If the expression on the right does not have a matching type, then the compiler will first search for any cast functions to perform an automatic cast.
```!asa
x : int = 5.283;  // This casts the float `5.283` to an int32
```

-----

## Blank initialization
If there is no `:::asa =` set operator, then the variable will be zero-initialized. This means something different depending on what the datatype is, see [zero values](#zero-values).
```!(blank_init.asa)asa
y : int;
printl(y);
```
```!shell
$ asa blank_init.asa
$ ./build/blank_init.asa
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
```#!(default_value.asa)asa
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
```!shell
$ asa default_value.asa
$ ./build/default_value.asa
6
hello!
```
Initializing to `:::asa default` gives the zero value for builtin types, or uses the default constructor for structs.

### `:::asa ?` or undefined
The symbol for undefined or unset data is `:::asa ?`. If you are heavily optimizing your software, or wish for a variable to not be initialized where it is declared for any reason, you can set it equal to this.


For example, leaving `:::asa x` undefined until much later in a program.
```!(undefined.asa)asa
x : int = ?;

// ...

x = 4;
```
This value is also used as the zero-value of pointers specifically, and is the only case where `:::asa ?` and `:::asa default` have the same value:
```!(undefined_pointer.asa)asa
a : *int = default;
b : *char = ?;

printl(int(a));  // 0
printl(int(b));  // 0
```
```!shell
$ asa undefined_pointer.asa
$ ./build/undefined_pointer.asa
0
0
```

### As values
Both `:::asa ?` and `:::asa default` are also usable as values wherever literals are allowed, and the type is known. For example:
```!(undefined_as_value.asa)asa
p : *int = ?;

if(p == ?)
    printl("Pointer is null");
```
```!shell
$ asa undefined_as_value.asa
$ ./build/undefined_as_value.asa
Pointer is null
```
Or:
```!(default_as_value.asa)asa
s : string = "";

if(s == default)
    printl("String is empty");
```
```!shell
$ asa default_as_value.asa
$ ./build/default_as_value.asa
String is empty
```

-----

## The `:::asa initial` value
There is another special value which acts as shorthand for the first "initial" value of a variable from its declaration.
```!(initial.asa)asa
x : int = 512;
printl(x);    // Prints 512

x = 29;
printl(x);    // Prints 29

x = initial;
printl(x);    // Prints 512
```
```!shell
$ asa initial.asa
$ ./build/initial.asa
512
29
512
```
Basically, `:::asa initial` holds the value the variable was set to in its declaration. If the declaration does not have an initial value, then it will throw an error. For example:
```asa
x : int;

x = initial;  // Throws a compilation error
```

