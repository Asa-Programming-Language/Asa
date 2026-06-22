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

## `:::asa #embed`

This is similar to `:::asa #import`, but instead of only including a single module, it loads the entire file. Also, it does so by an explicit path. For example:
```asa
#embed "./otherFile.asa";
```
This would compile and import the entire other file at the given path. The path is evaluated relative to the file which `:::asa #embed` is in.

-----

## `:::asa #library`

Used to specify the name of a library to link against. For example:
```asa
#library "ssl";
```
This would link against libssl. It is equivalent to passing `-lssl` with `--clangoptions`.

-----

## `:::asa #library_static`

Used to specify the name of a library to statically link against. For example:
```asa
#library_static "/path/to/libssl.a";
```
This would link against libssl. It is equivalent to passing `/path/to/libssl.a` with `--clangoptions`.

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

## `:::asa #filepath`

Gets the full path of the source code file as a string. For example:
```asa
// In a file at /home/user/main.asa
print(#filepath);
```
The above would print `/home/user/main.asa`.

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

## `:::asa #compiles(expression)`

Checks at compile time if `expression` will compile successfully, or cause a compiler error. Returns `:::asa true` if it compiled without error, and `:::asa false` if it failed. The error message is thrown out, and never printed to stdout or stderr.
```asa
printl(#compiles(1 + 1)); // Prints `true`
printl(#compiles(undefinedVariable + 1)); // Prints `false`
```

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

-----

## `:::asa #set_attribute(ast_node, attribute_name, value)`

Sets or changes the given AST node's attribute to the new value
```asa
@public:
x : int = 0;

#set_attribute(#parent(x), "public", false);
```

-----

## `:::asa #setflag(name, value)`

Sets a **global** flag to the given value.
```asa
#setflag(FOO, true);
// OR:
#setflag FOO true;
```

-----

## `:::asa #getflag(name)`

Returns the value of the named **global** flag.
```asa
#getflag(FOO);
```

-----

## `:::asa #recommend_options(string)`

Sets compiler options at compile time. For security, it asks the user with a prompt before applying, and the user can reject the options. Uses the same syntax as options applied through the command line:
```asa
#recommend_options("-O3 -w all");  // Applies optimization level 3 and enables all warnings
```
The same as this command:
```
asa main.asa -O3 -w all
```

-----

## `:::asa #error(string, ast_node)`

Causes a compiler error at compile time, using the same error system that the compiler uses for it's builtin errors. It uses the `string` as the error message, and the `ast_node` as the line context shown in the error message.
```asa
#error("This is an error", SOME_AST);
```
!!! note
    This requires that the second argument is a reference to an AST node. It will work if you pass something else, such as a variable name: `:::asa #error("...", x);`. But it will be a new node generated for this line, rather than for the definition or set of `:::asa x`

-----

## `:::asa #warning(string, ast_node)`

Causes a compiler warning at compile time, using the same error system that the compiler uses for it's builtin errors. It uses the `string` as the warning message, and the `ast_node` as the line context shown in the warning message.
```asa
#warning("This is a warning", SOME_AST);
```
!!! note
    This requires that the second argument is a reference to an AST node. It will work if you pass something else, such as a variable name: `:::asa #warning("...", x);`. But it will be a new node generated for this line, rather than for the definition or set of `:::asa x`

-----

## `:::asa #stack_push(string, ast_node)`

This pushes a given AST node to the named stack. If the stack does not exist yet, it creates a new one. Example:
```asa
#stack_push("some stack", true);
```
!!! note
    Like mentioned above, the second argument is an AST node. If you pass a literal or expression, this will pass a new AST node representing that expression.

-----

## `:::asa #stack_pop(string)`

This pops a single item off of the given stack. It does not return anything.
```asa
// "some stack" => {0, 4, 2, 1}

#stack_pop("some stack");

// "some stack" => {0, 4, 2}
```

-----

## `:::asa #stack_last(string)`

This returns the last AST node added to the named stack.
```asa
// "some stack" => {0, 4, 2, 1}

printl(#stack_last("some stack"));  // Prints: 1
```

-----

## `:::asa #print_ast(ast_node)`

This prints out the given AST node to the terminal.
```asa
x :: 4;

#print_ast(x);

// Prints:
// :(Expression_Term){
//     6:(Integer_Node){}
// }
```
!!! note inline
    For compile time equals definitions using `:::asa ::`, it is the AST of its value, rather than its definition. 

-----

## `:::asa #parent(ast_node)`

This returns the parent node of the given AST node. You can use this for cases like above, where you want the AST node of the definition rather than the right-hand-side.
```asa
x :: 4;

#print_ast(#parent(x));

// Prints:
// x:(Compiler_Define){
//     :(Expression_Term){
//         6:(Integer_Node){}
//     }
// }
```

-----

## `:::asa #print(string)`

Prints the given string to the terminal at compile time.
```asa
#print("This prints only during compilation");
```

-----

## `:::asa #printl(string)`

Prints the given string to the terminal at compile time with a newline.
```asa
#printl("This prints only during compilation");
```

-----

## `:::asa #if(condition, ast_node)`

Evaluates `condition` as a boolean. If it is "true", the `ast_node` is included in compilation. If it is "false", it is ignored.
```asa
#if(true, printl(4));  // The print statement is included in the compiled program
```
```asa
x :: { i : int = 9 };
#if(1 == 2, x);  // The code `i : int = 9` is not included in the program
```

-----

## `:::asa #make_directive(name, (ast_arguments), ast_node)`

Creates a new custom compiler directive with the given name, arguments, and body.
```asa
#make_directive("custom_add", (A, B), {
    #return A + B;
});

#custom_add(1, 2);  // Returns AST node of {1+2}
```

-----

## `:::asa #return(ast_node)`

Used in conjunction with `:::asa #make_directive` to let the expression result in a given AST node.
```asa
#make_directive("custom_add", (A, B), {
    #return A + B;
});
```

-----

## `:::asa #context`

Returns the AST node of the current context. For example, if used inside a `::asa #make_directive` definition, `:::asa #context` will be the node where it is called.
```asa
#make_directive("custom_add", (A, B), {
    #printast(#context);
});

#custom_add(1, 2);  // `#context` refers to this line
```
