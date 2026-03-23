%%% order 1

# Compiler Directives

In Asa, there are a number of directives for modifying the behavior of the compiler during compilation. These all start with the character `:::asa #`. Any compiler directive is an expression that will be evaluated at **compile time**.

----

## `:::asa #import`

One of the most used directives, used for importing modules by name. For example:
```asa
#import Rendering.Window;
#import Rendering.Drawing.Line;
```
This imports the module `Window` in the directory `modules/Rendering`. The module expression can be more complex, for example sub-directories: `:::asa #import A.B.C.ModuleName;`, which would be in `modules/A/B/C`. You can also wildcard import modules by using the base path followed by an asterisk: `:::asa #import Builtin.*;`, which would import all modules of all files in the directory `modules/Builtin/`.

`:::asa #import` looks in multiple locations for the specified module. First, it looks in the local directory from where it is called. If there is a matching module (including all path components), then it will stop there. If it does not find a matching module locally, it will look in the global modules folder, which is installed next to the `asa` executable. This allows you to override builtin modules for specific use cases.

-----

## `:::asa #file`

This is similar to `:::asa #import`, but instead of only including a single module, it loads the entire file. Also, it does so by an explicit path. For example:
```asa
#file "./otherFile.asa";
```
This would compile and import the entire other file at the given path. The path is evaluated relative to the file which `:::asa #file` is in.

-----

## `:::asa #linenum`

Gets the line number as an integer of its own location in the source code file. For example:
```#(3)asa
// some line
print(#linenum); // Prints 4
```

-----

## `:::asa #linecol`

Gets the line column as an integer of its own location in the source code file. For example:
```asa
print(#linecol); // Prints 6
```

-----

## `:::asa #line`

Gets the line contents as a string at its own location in the source code file. For example:
```asa
print(#line);
```
The above prints `print(#line);`. (Putting this in a comment would be recursive.)

-----

## `:::asa #filename`

Gets the name of the source code file as a string. For example:
```asa
// In a file called main.asa
print(#filename);
```
The above would print `main.asa`.

-----

## `:::asa #funcname`

Gets the name of the function it is in as a string. For example:
```asa
someFunc :: int(){
    print(#funcname);
    ...
}
```
The above would print `someFunc`.

-----

## `:::asa #modulename`

Gets the name of the immediate module it is in as a string. For example:
```asa
moduleA :: module{
    print(#modulename);
}
```
The above would print `moduleA`. But in the case of nested modules like so:
```asa
moduleA :: module{
    moduleB :: module{
        print(#modulename);
    }
}
```
The above would print `moduleB`.

-----

## `:::asa #asaversion`

Gets the version of the Asa compiler as a string as it was when compiled. For example:
```asa
print(#asaversion);
```
The above would print the current version string of the asa compiler.

-----

## `:::asa #counter`

Acts as an automatically incrementing integer. For example:
```asa
print(#counter); // Prints 0
print(#counter); // Prints 1
print(#counter); // Prints 2
```

-----

## `:::asa #nameof(I)`

Gets the name of an identifier as a string. For example:
```asa
someVar : int = 0;
print(#nameof(someVar));
```
The above would print the string `someVar`.

-----

## `:::asa #typeof(T)`

Gets the type name of an identifier or type as a string. For example:
```asa
someVar : int = 0;
print(#typeof(someVar)); // Would print `int`
print(#typeof(string));  // Would print `string`
```

-----

## `:::asa #sizeof(T)`

Gets the size in bytes of an identifier or type as an integer. For example:
```asa
someVar : int = 0;
print(#sizeof(someVar)); // Would print `4`
print(#sizeof(int8));    // Would print `1`
```

-----

## `:::asa #extern`

This is a method to reference external functions from libraries. For example:
```asa
#extern printf :: int32 (s : const *char, ...);
```
The above would allow you to use the `printf` function from the C standard library. `:::asa #extern` use is simply stating the function name and parameters as they are defined, and not including a body.

-----

## `:::asa #inline`

This is a function modifier that tells the compiler it should be inlined for performance.

-----

## `:::asa #replaceable`

This is a function modifier that tells the compiler another function with the same identity can be defined. While function overloading typically requires a different identity, if `:::asa #replaceable` is used in the original, it can be overloaded without an error. This will effectively delete the `:::asa #replaceable` function and use the newly defined one.

-----

## `:::asa #hideast`

This is a function modifier that hides the function from showing in the AST verbose output. This is primarily used for compiler development.

-----

## `:::asa #variant`

This is a function modifier that creates variants of the function. For example:
```asa
foo :: ()
    #variant w = 0;
{
    printl(w);
}
```
Then you can use it like so:
```asa
foo<w=7>(); // -> 7
foo<w=2>(); // -> 2
```
This looks like arguments with extra steps, but the main difference is that it permanently creates a completely different function for each combination during the compilation process. So the above would have equivalent code to:
```asa
foo :: (){
    printl(7);
}

foo :: (){
    printl(2);
}
```
This is useful if you want to allow for a function to work with multiple types:
```asa
foo :: T(v : T)
    #variant T = int;
{
    return v * 2;
}
```
