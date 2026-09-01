%%% order 1

# Introduction to Asa
>>> beginner 

Asa is a systems programming language built to be used in a similar fashion to C++, but be a good replacement. It aims to improve upon many of the shortcomings of that aging and bloated language, without sacrificing performance.

## Hello World!

TODO: Update this with whatever imports are actually needed, when the std library is more organized. Currently, importing all with `Builtin.*` should get the `printl` function.
```!(hello.asa)asa
#import Builtin.*;

main :: (){
    printl("Hello World!");
}
```

```!shell
$ asa ./hello.asa
$ ./build/hello
Hello World!
```

See:
* [#import](compiler_directives.html#-import)
* [printl](Print.html)
* [Compile time define operator](#the-compile-time-define-operator)

---

## Comments

Comments in Asa follow the same rules as those in C/C++.

Single line comments begin with two forward slashes: `:::asa //`
```!asa
// This is a comment
```
Everything before the `//` is considered code, and everything after is considered a comment, until it reaches the end of the line:
```!asa
printl("this is code!");  // this is a comment
printl("back to code again!");
```
Multi-line comments begin with `/*`, and end with `*/`
```!asa
/* This is a multi
        line
             comment!
*/
```
Everything between `/*` and `*/` is considered a comment, including newlines. The comment only ends when it reaches the close symbol, `*/`.

Due to how multi-line comments are parsed, this means that you can also place them within lines of code without issue:
```!asa
if (/* some spurious comment */ true){
}
```

----

## Identifiers

Identifiers in Asa may contain any alphanumeric character or underscore, but must begin with an alphabetic character or underscore.
```!asa
this_is_an_identifier = 5;
thisIsAlso = 2;
_this2 = 1;
EVEN_THIS = 6;
```

----

## Builtin Operators

TODO: This is a stub, and does not contain all operators.
| Action | Symbol | Description | Example |
|--------|--------|--------|---------|
| Addition        | `:::asa +`<br>`:::asa +=`   | Adds two values together                                      | `:::asa 1 + 2 == 3;`<br>`:::asa x += 2;`                   |
| Subtraction     | `:::asa -`<br>`:::asa -=`   | Subtracts one value from another                              | `:::asa 5 - 3 == 2;`<br>`:::asa x -= 2;`                   |
| Negation        | `:::asa -`                  | Gives the opposite signed value                               | `:::asa x = -3;`<br>`:::asa -3 == 0-3;`                    |
| Multiplication  | `:::asa *`<br>`:::asa *=`   | Multiplies one value by another                               | `:::asa 4 * 3 == 12;`<br>`:::asa x *= 5;`                  |
| Division        | `:::asa /`<br>`:::asa /=`   | Divides one value by another                                  | `:::asa 12 / 3 == 4;`<br>`:::asa x /= 2;`                  |
| Modulo          | `:::asa %`<br>`:::asa %=`   | Gives the remainder after division                            | `:::asa 5 % 3 == 2;`<br>`:::asa x %= 2;`                   |
| Left bit shift  | `:::asa <<`<br>`:::asa <<=` | Shifts the bits of the value to the left by the given amount  | `:::asa 2 << 1 == 4;`<br>`:::asa x <<= 3;`                 |
| Right bit shift | `:::asa >>`<br>`:::asa >>=` | Shifts the bits of the value to the right by the given amount | `:::asa 8 >> 1 == 4;`<br>`:::asa x >>= 2;`                 |
| Bitwise And     | `:::asa &`<br>`:::asa &=`   | Performs a logical bitwise AND on the bits of two values      | `:::asa 0b101 & 0b111 == 0b101;`<br>`:::asa x &= 0b1001;`  |
| Bitwise Or      | `:::asa \|`<br>`:::asa \|=` | Performs a logical bitwise OR on the bits of two values       | `:::asa 0b100 \| 0b011 == 0b111;`<br>`:::asa x \|= 0b01;`  |
| Bitwise Xor     | `:::asa ^`<br>`:::asa ^=`   | Performs a logical bitwise XOR on the bits of two values      | `:::asa 0b101 ^ 0b011 == 0b110;`<br>`:::asa x ^= 0b01;`    |
| Bitwise Not     | `:::asa ~`<br>`:::asa ~=`   | Performs a logical bitwise NOT on the bits of a value         | `:::asa ~0b011 == 0b100;`<br>`:::asa x ~= 0b01;`           |

----

## The Compile Time Define Operator

One of the operators you will be using most often in Asa is the compile time define. This is the double colon, `:::asa ::`.


It shares many of the characteristics of the set operator (`:::asa =`), and sometimes their functionality even overlaps.

Importantly, anything defined with this operator is **immutable**. This means that it cannot change during runtime. But, that does *not* mean it cannot be *re*-defined at compile time. In that way, it can somewhat also be compared to the `:::asa #define` preprocessor keyword from C or C++.


Compile time define is used to assign any expression to a name. A very basic example would be:
```!asa
x :: 5;
```
This code will evaluate the exact same as:
```!asa
x : const = 5;
```
See [type modifiers]()

Of course, more interesting expressions are better. Let's put a lambda expression on the right side:
```!asa
x :: int(){ return 5; }
```
Now this is a function.

### Other uses

`:::asa ::` is used for defining other things as well, such as structs, modules, and special functions.

See:
* [Structs](#structs)
* [Modules](#modules)

----

## Structs

You can create a struct by writing the name, and defining it as a struct expression:

```!asa
someStruct :: struct{
    // ...
}
```
Then to create an instance:
```!asa
y : someStruct = someStruct();
```

### Struct Members

Member variables and functions are defined as normal. Functions, though, have slightly different behavior. For example:

```#!(struct_members.asa)asa
someStruct :: struct{

    x : int = 10;

    memberFunction :: (v : int){
        this.x = v;
    }
}
```
The difference with functions defined inside of a struct is that they can only be called with the access operator (`:::asa .`), but they can also access members within the struct itself. To do this, you must use the `:::asa this` keyword, which references the instance the function is within.
Then member access is as normal:
```!(struct_members.asa (continued))#(9)asa
s : someStruct = someStruct();

printl(s.x); // -> 10

s.memberFunction(3);

printl(s.x); // -> 3
```

```!shell
$ asa struct_members.asa
$ ./build/struct_members
10
3
```

### Struct Specific Functions

There are some functions that have specific names and functionality for handling structs.

#### `:::asa create`

```!asa
create :: someStruct(){
    // ...
}
```

----

## Modules

You can define a module by writing the name, and defining it as a module expression:

```!asa
moduleName :: module{

}
```

Modules can have sub-modules to any depth:

```!asa
moduleA :: module{
    moduleB :: module{
        moduleC :: module{

        }
    }
}
```

### Accessing

You can use the access operator `:::asa .` to use members of a module.

```!asa
moduleA :: module{
    moduleB :: module{
        x = 4;
    }
}
```

Accessing the variable `x` from outside `moduleA` would be done like so:

```!asa
moduleA.moduleB.x = 5;
```

----

## Expressions

In Asa, most things you write are "expressions". In other words, the individual components should be able to be evaluated all on their own. For example, take the following function:

```!asa
functionName :: (){
}
```

Given the above function, we can split it into its sub-expressions:

The name:
```asa
functionName
```
and a lambda:
```asa
(){}
```

The lambda is simply an unnamed function. You can create one with parentheses containing the function arguments, followed by curly braces containing the body. Like: 
```!asa 
(x : int){}
```
If you want the lambda to have a return value, it should be preceded by the type. Like:
```!asa 
int(x : int){ return x+1; }
```

Now, neither of those pieces of code do anything. So giving that lambda expression a name with the compiler define operator `:::asa ::` lets you easily use it anywhere:
```!asa 
someFunc :: int(x : int){ return x+1; }

someFunc(3); // Returns 4
```

But the name `:::asa someFunc` is just an alias for the lambda expression. You could have evaluated it by just appending parenthesis to the end of the lambda itself and passing the arguments:
```!asa 
someFunc :: int(x : int){ return x+1; }(3);  // Returns 4
```

----

## Special Function Definitions

Just like in C++, there are some special ways to define functions for certain use cases.

### Operator Overloading

Operator overloading is used to override builtin behavior or add new behavior to existing or new symbols. For example:

```!(operator_overload.asa)asa
operator(@) :: int(x : int, y : int){
    return x * y;
}

printl(3 @ 2);
```
```!shell
$ asa operator_overload.asa
$ ./build/operator_overload
6
```

Defining an operator overload is done with the following syntax:

```
operator(<symbol>) :: <return type>(<Left value>, <Right value>){
}
```

The `<symbol>` can be any overloadable operator token. Examples: `:::asa +`, `:::asa ==`, `:::asa []`, or `:::asa ..`. The compile time define `:::asa ::` and a few punctuation symbols cannot be overloaded. Also, the list of available symbols is predefined, so some combinations may be missing.

----

## Strings

Strings are defined in the [String module](String.html). They are simply an address to a null-terminated character array, and a length:
```!asa
// This is all a string is:
string :: struct {
    address : *char = ?;
    length : uint32 = ?;
}
```

You can declare a string variable like any other struct:
```!asa
s : string;
```

And you can use string literals to create a string with a value:
```!asa
s : string = "This is a string!";
```

A string literal is just any text surrounded by double quotes.

### Formatted Strings

Like a regular string, formatted strings, or f-strings for short, are surrounded by quotes. But f-strings are also preceded by an `f` character:

```!asa
s : string = f"this is an f-string";
```

They are not a different data type from `string`, rather they are just a different way to process string literals. They allow you to easily concatenate data with strings in a much shorter way.

For example, the following is a standard way to insert multiple pieces of data into a string:

```!(manual_concatenation.asa)asa
x : int = 5;
y : float = 1.1;
z : string = "other str";

combination : string = "The combined values are: x:" + string(x) + ", y:" + string(y) + ", z:" + z;
```
```!shell
$ asa manual_concatenation.asa
$ ./build/manual_concatenation
The combined values are: x:5, y:1.1, z:other str
```

This is a lot of code to do something so simple! So instead we can use an f-string:
```!(fstring_concatenation.asa)asa
x : int = 5;
y : float = 1.1;
z : string = "other str";

combination : string = f"The combined values are: x:{x}, y:{y}, z:{z}";
```
```!shell
$ asa fstring_concatenation.asa
$ ./build/fstring_concatenation
The combined values are: x:5, y:1.1, z:other str
```

The output is the same, but we no longer need to manually cast the values into strings, that is done automatically. Also, we do not need to manually concatenate multiple strings together using `:::asa +`. Just put the expression in between curly braces, like `:::asa {5+3}`, and it will be evaluated, casted to string, and concatenated in place.


F-strings do not give any performance benefit compared to its manual counterpart, as it is essentially expanding into the manual form under the hood.

