; Preprocessor & comments
(comment) @comment
(preproc_include) @keyword.import
(preproc_directive) @keyword.directive
(system_lib_string) @string.special

; Keywords & types
[
  "component"
  "typedef"
  "struct"
  "union"
  "enum"
  "return"
  "if"
  "else"
  "for"
  "while"
  "break"
  "continue"
  "sizeof"
] @keyword

(storage_class_specifier) @keyword.modifier
(type_qualifier) @keyword.modifier
(primitive_type) @type.builtin

; Components & functions
(component_definition name: (component_identifier) @type.definition)
(component_declaration name: (component_identifier) @type.definition)
(call_expression function: (identifier) @function.call)
(field_expression field: (identifier) @property)

; JSX tags, components & attributes
(jsx_tag_name) @tag
(jsx_component_name) @type
(jsx_attribute_name) @attribute
((jsx_attribute_name) @function.method
  (#match? @function.method "^on[A-Z]"))
(jsx_text) @string.special

; Closures
(capture_list) @punctuation.bracket

; Literals
(string_literal) @string
(char_literal) @character
(number_literal) @number
(null_literal) @constant.builtin
(true_literal) @boolean
(false_literal) @boolean

; Punctuation
[
  "(" ")" "[" "]" "{" "}" "<" ">" "</" "/>" "<>" "</>"
] @punctuation.bracket

[
  "," ";" "." "->"
] @punctuation.delimiter
