
# Todo:

- [ ] Implement `#defined()` compiler directive

- [ ] Make enum names act as type name

- [ ] Disallow constructors from modifying global state

- [ ] Make all compilation be executed using a singular function path, to prevent repetition and multiple steps that must be kept up to date separately

- [ ] Add compiler multi-error handling. Continues codegen/parsing unless it relies on a previously errored node
    - [ ] Add AST node poisoning  

- [ ] Disallow defining compile time defined symbols using non constant expression.

- [ ] Fix operator overload token string being overwritten by token name

- [ ] Add `#export_name(string)` compiler directive

- [ ] Allow attributes to have empty argument lists. For example, while `@deprecated:` compiles, and `@deprecated("message"):` compiles, `@deprecated():` does not.

- [ ] Make error checking ensure all code paths have a return value if necessary

- [ ] Make modules based completely in AST nodes

- [ ] Ensure `result` statement works in loops and most other scope bodies

- [ ] Add `defer` keyword or directive to push a statement to the end of a scope. This involves pusing each deferred expression onto a stack, and executing in reverse order at the end of the current scope. And if the current scope is a function or a `result` returning scope, then the return value should be evaluated first, and then the deferred expressions executed. And they should be executed after any instances of the return/result keyword.

- [ ] Think about disallowing certain function/operator overloads, adding checking for patterns. For example, disallowing anything other than integer in operator[], and erroring. And error when you try to overload important operators like `=`

- [ ] Make variants use `$` for variant names. Like: `someFunc<$T : type> :: $T(){}`

- [ ] Add `@command_option` or `#command`. To allow creating program options from functions

- [ ] Add `#embed_binary` compiler directive

- [ ] Allow setting specific type of enum values, ie. `uint16` or `float`

- [ ] Implement function multi-return

- [ ] Add way to get all attributes of symbol as array for iterating over

- [ ] Add `@callingconvention("c"):` attribute

- [ ] Make lockfile or similar system to prevent two instances of asa from compiling something at once



# Done:
- [x] Fix `initial`, doesnt work with global variables.
- [x] Ensure compiler directives have exact number of expected arguments, and errors on not enough or too many arguments
- [x] Make functions able to return a reference. Returning `ref T` allows using return as lvalue, modifying like `foo().x = 5;`. Returning `const ref T` allows only using it as readonly. This is necessary for things like `operator[]`, where getting and setting is expected to have different behavior. Like `someMap[5] = 4;` and `printl(someMap[5]);`   TODO: See if this differentiation is actually necessary
- [x] Re-implement file.asa
- [x] Add **builtin** array type
- [x] Fix `operator[]`.
- [x] Add builtin type documentation
- [x] Implement string formatting with internal curly braces: `someVar : int = 5;` then `printl(f"{someVar}");` -> `"5"`
- [x] Improve declaration initialization, adding `default` and `?` values
- [x] Add compiler directives: `#if(COND, BODY)`, `#stack_last(NAME)`, `#stack_push(NAME, AST)`, `#stack_pop(NAME)`, `#error(MESSAGE, AST)`, `#warning(MESSAGE, AST)`, `#context`
- [x] Add `#recommend_options` compiler directive to allow programs to suggest compilation options. When reached, the compiler will prompt the user if they would like to accept the options or not.
- [x] Error on attempting to modify a compiler constant. Like `SOME_CONST :: 5;`  `SOME_CONST = 0;`
- [x] Fix If/ If else chaining silently failing
- [x] Fix function definition multi-line arguments
- [x] Add `array` type
- [X] Improve error printing system, make it allow multiple underlined segments with attached messages and connection lines
