We follow the [Google C++ Style Guide](http://google-styleguide.googlecode.com/svn/trunk/cppguide.xml) with the following differences:

## Naming

### Variable Names
- We use [lowerCamelCase](http://en.wikipedia.org/wiki/CamelCase#Variations_and_synonyms) for variable names (Google uses snake_case, and their class member variables have trailing underscores).

### Constant Names
- We use lowerCamelCase for constant names (Google uses a `k` followed by mixed case, e.g. `kDaysInAWeek`).

### Function Names
- We use lowerCamelCase for function names (Google uses mixed case for regular functions; and their accessors and mutators match the name of the variable).

## Strings
- Strings used in log and error messages should end without a period.

## Comments
- End each sentence with a period.
- At most 70 characters per line in comments.

## Indentation
- New line when calling or defining a function: indent with 4 spaces.
- Other cases: indent with 2 spaces.

## New Lines
- 1 blank line at the end of the file.
- Elements outside classes (classes, structs, global functions, etc.) should be spaced apart by 2 blank lines.
- Elements inside classes (member variables and functions) should not be spaced apart by more than 1 blank line.
