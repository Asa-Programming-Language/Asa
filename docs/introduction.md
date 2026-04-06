%%% order 0

# Introduction to Asa

!!! important
    This page is very much still a work in progress

Asa is a systems programming language built to be used similarly to C++, but be a good replacement. It aims to improve upon many of the shortcomings of that aging language, without sacrificing performance.

```asa
main :: (){
    printl("Hello World!");
}
```

---

## The Compile Time Define Operator

One of the operators you will be using most often in Asa is the compile time define. This is defined as the double colon, `:::asa ::`.
It shares many of the characteristics of the set operator (`:::asa =`), and sometimes their functionality even overlaps. It can also be compared to the `:::asa #define` preprocessor keyword from C or C++, although Asa does not have a preprocessor.

Compile time define is used to assign an expression to a name. The most basic example would be:
```asa
x :: 5;
```
!!! note inline
    This code will evaluate to the exact same as `:::asa x = 5;`

Of course, more interesting expressions are better. Let's put a lambda expression on the right side:
```asa
x :: int(){ return 5; }
```
Now this is a function.

### Other uses

`:::asa ::` can be used for defining other things as well, such as structs, modules, and special functions.

#### Defining Structs

You can create a struct by writing the name, and defining it as a struct expression:

```asa
someStruct :: struct{

}
```

#### Defining Modules

You can define a module by writing the name, and defining it as a module expression:

```asa
moduleName :: module{

}
```

----

## Structs

You can create a struct by writing the name, and defining it as a struct expression:

```asa
someStruct :: struct{
    // ...
}
```
Then to create an instance:
```asa
y : someStruct = someStruct();
```

### Struct Members

Member variables and functions are defined as normal. Functions, though, have slightly different behavior. For example:

```#asa
someStruct :: struct{

    x : int = 10;

    memberFunction :: (v : int){
        this.x = v;
    }
}
```
The difference with functions defined inside of a struct is that they can only be called with the access operator (`:::asa .`), but they can also access members within the struct itself. To do this, you must use the `:::asa this` keyword, which references the instance the function is within.
Then member access is as normal:
```#(9)asa
s : someStruct = someStruct();

printl(s.x); // -> 10

s.memberFunction(3);

printl(s.x); // -> 3
```

### Struct Specific Functions

There are some functions that have specific names and functionality for handling structs.

#### `:::asa create`

```asa
create :: someStruct(){
    // ...
}
```

----

## Modules

You can define a module by writing the name, and defining it as a module expression:

```asa
moduleName :: module{

}
```

Modules can have sub-modules to any depth:

```asa
moduleA :: module{
    moduleB :: module{
        moduleC :: module{

        }
    }
}
```

### Accessing

You can use the access operator `:::asa .` to use members of a module.

```asa
moduleA :: module{
    moduleB :: module{
        x = 4;
    }
}
```

Accessing the variable `x` from outside `moduleA` would be done like so:

```asa
moduleA.moduleB.x = 5;
```

----

## Expressions

In Asa, most things you write are "expressions". In other words, the individual components should be able to be evaluated all on their own. For example, take the following function:

```asa
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

The lambda is simply an unnamed function. You can create one with parentheses containing the function arguments, followed by curly braces containing the body. Like: `:::asa (x : int){}`.
If you want the lambda to have a return value, it should be preceded by the type. Like: `:::asa int(x : int){ return x+1; }`

----

## Special Function Definitions

Just like in C++, there are some special ways to define functions for certain use cases.

### Operator Overloading

Operator overloading is used to override builtin behavior or add new behavior to existing or new symbols. For example:

```asa
operator+- :: int(x : int, y : int){
    return x * y;
}

printl(3 +- 1);
// -> Outputs 3
```

Defining an operator overload is done with the following syntax:

```
operator<symbol> :: <return type>(<Left value>, <Right value>){
}
```

The `<symbol>` can be any ASCII special character, and can be a double character as well. Example: `:::asa $` or `:::asa $$`. The only symbols you cannot overload are the compile time define `:::asa ::` and a few punctuation symbols. Also, the list of available symbols is predefined, so some combinations may be missing.
