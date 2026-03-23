%%% parent attributes.md

# Accessing
You can get the value of an objects attributes at compile time using the attribute access operator `.@`.

----

For example:

Attributes without an argument will have the value `:::asa true` if they are present, and `:::asa false` otherwise.

```asa
@public:
foo ::(){
}

bar ::(){
}
```

```asa
printl(foo.@public);  // Prints `true`

printl(bar.@public);  // Prints `false`
```

-----

Attributes with an argument will return the value they contain if they are present, and false if they are not present.

```asa
@deprecated("Some message"):
foo ::(){
}

@customAttr("example"):
bar ::(){
}
```

```asa
printl(foo.@deprecated);  // Prints `Some message`

printl(bar.@customAttr);  // Prints `example`

printl(bar.@deprecated);  // Prints `false`
```
