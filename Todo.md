
# Todo:

- [ ] Fix operator overload token string being overwritten by token name

- [ ] Add `defer` keyword to push a statement to the end of a scope. This involves pusing each deferred expression onto a stack, and executing in reverse order at the end of the current scope. And if the current scope is a function or a `result` returning scope, then the return value should be evaluated first, and then the deferred expressions executed. And they should be executed after any instances of the return/result keyword.

- [ ] Make modules based completely in AST nodes

- [ ] Add compiler directives: `#if(COND, BODY)`, `#stack_last(NAME)`, `#stack_push(NAME, AST)`, `#stack_pop(NAME)`, `#error(MESSAGE, AST)`, `#warning(MESSAGE, AST)`, `#context`

- [ ] Improve error printing system, make it allow multiple underlined segments with attached messages and connection lines

- [ ] Ensure `result` statement works in loops and most other scope bodies

- [ ] Make error checking ensure all code paths have a return value if necessary

- [ ] Think about disallowing certain function/operator overloads, adding checking for patterns. For example, disallowing anything other than integer in operator[], and erroring. And error when you try to overload important operators like operator=

- [ ] Add compiler multi-error handling. Continues codegen/parsing unless it relies on a previously errored node
    - [ ] Add AST node poisoning  

- [ ] Make all compilation be executed using a singular function path, to prevent repetition and multiple steps that must be kept up to date separately

- [ ] Make variants use `$` for variant names. Like: `someFunc<$T : type> :: $T(){}`

- [ ] Make enum names act as type name

- [ ] Add `#embed_binary` compiler directive

- [ ] Fix `operator[]`.

- [ ] Allow setting specific type of enum values, ie. `uint16` or `float`

- [ ] Implement function multi-return

- [ ] Improve declaration initialization, adding `default` and `?` values

- [ ] Make functions able to return a reference. Returning `ref T` allows using return as lvalue, modifying like `foo().x = 5;`. Returning `const ref T` allows only using it as readonly. This is necessary for things like `operator[]`, where getting and setting is expected to have different behavior. Like `someMap[5] = 4;` and `printl(someMap[5]);`   TODO: See if this is actually necessary

- [ ] Add way to get all attributes of symbol as array for iterating over

- [ ] Add **builtin** array type

- [ ] Add `@command_option` or `#command`

- [ ] Add `@callingconvention("c"):` attribute

- [ ] Make lockfile or similar system to prevent two instances of asa from compiling something at once

- [ ] Allow attributes to have empty argument lists. For example, while `@deprecated:` compiles, and `@deprecated("message"):` compiles, `@deprecated:` does not.




# Done:
- [x] Add `array` type
