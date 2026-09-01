# Attributes
Attributes attached to a module, function, variable, or other referenceable object, which change how it is seen by the compiler, and attach constants.

For example:
```asa
@public:
foo :: (){

}
```

Or with an argument passed:
```asa
@someAttribute("Some value"):
foo :: (){

}
```

!!! important
    An attribute can only have literals as an argument/value.
---

# Common Attributes:
>>> beginner

-----

## `@public`
Makes a referenceable object accessible from outside the scope.

-----

## `@private`
Marks an object as private. Only acts as label and check, most things are private by default.

-----

## `@deprecated(string)`
Marks an object as deprecated. If it is used anywhere, the compiler will warn that it is deprecated, and if a message is provided as a string, print that as well.

-----

## `@removed(string)`
Marks an object as removed. Stronger version of `:::asa @deprecated`. If it is used anywhere, the compiler will throw an error, and if a message is provided as a string, print that as well.

-----

## `@inline`
Hints that a function should be inlined wherever used.

---


# Advanced Attributes:
>>> advanced

-----

## `@replaceable`

Tells the compiler that another function with the same identity can be defined later in the program to overwrite it. While function overloading typically requires a different identity, if `:::asa @replaceable` is an attribute of the original, it can be overloaded without an error. This will effectively delete the `:::asa @replaceable` function and use the newly defined one.

-----

## `@hideast`
Tells the compiler to not show this object in the printed AST.

-----

## `@effectless`
Tells the compiler that this function will not modify global state, no side effects.

-----

## `@internal`
Tells the compiler that this object will not be referenced from outside this binary. Allows for better optimization. Most symbols are marked `@internal` by default.

-----

## `@external`
Tells the compiler that this object may be referenced from outside this binary.

-----

## `@scoped`
TODO: This attribute may not be implemented if the module scoping stays as it is.

Makes a module's items only accessible if they are accessed from the module's name.
For example:
```asa
regularModule :: module{
    x = 0;
}

@scoped:
scopedModule :: module{
    x = 0;
}
```
Now this is how you access the variable imported from each module:
```asa
// Regular:
#import regularModule;
x += 1; // Imports normally
```
```asa
// Scoped:
#import_qualified scopedModule;
scopedModule.x += 1; // Requires full name
```

-----

