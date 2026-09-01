%%% parent attributes.md

# Accessing Attribute Values
>>> intermediate
You can get the value of an objects attributes at compile time using the attribute access operator `.@`.

----

For example:

Attributes without an argument will have the value `:::asa true` if they are present, and `:::asa false` otherwise.

```!(attributes.asa)asa
@public:
foo ::(){
}

bar ::(){
}

printl(foo.@public);
printl(bar.@public);
```

```!shell
$ asa attributes.asa
$ ./build/attributes
true
false
```

-----

Attributes with an argument will return the value they contain if they are present, and false if they are not present.

```!(attr_arguments.asa)asa
@deprecated("Some message"):
foo ::(){
}

@customAttr("example"):
bar ::(){
}

printl(foo.@deprecated);
printl(bar.@customAttr);
printl(bar.@deprecated);
```

```!shell
$ asa attr_arguments.asa
$ ./build/attr_arguments
Some message
example
false
```

----

While it is easy to ***get*** the value of an attribute, you can not use the same method to ***change*** the value of an attribute.
```!(attr_set.asa)asa
@customAttr("example"):
bar ::(){
}

bar.@customAttr = "changed";
```
TODO: Place the actual error message here
```!shell
$ asa attr_set.asa
$ ./build/attr_set
error[E007]:  Cannot modify value of attribute using access operator `.@`. Did you mean to use `#set_attribute()`?
```

Because attributes are considered immutable, modifying them at compile time is delegated to compiler directives for meta programming. See [#set_attribute](compiler_directives.html#-set-attribute-ast-node--attribute-name--value-).
