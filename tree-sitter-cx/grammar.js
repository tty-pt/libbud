/**
 * @file grammar.js
 * @brief Self-contained Tree-sitter grammar for `.cx` (C11 + JSX + `component` + closures).
 */

const PREC = {
  ASSIGN: 1,
  TERNARY: 2,
  OR: 3,
  AND: 4,
  BITOR: 5,
  BITXOR: 6,
  BITAND: 7,
  EQ: 8,
  REL: 9,
  SHIFT: 10,
  ADD: 11,
  MUL: 12,
  CAST: 13,
  UNARY: 14,
  CALL: 15,
  MEMBER: 16,
  JSX: 17,
};

module.exports = grammar({
  name: 'cx',

  extras: $ => [
    /\s/,
    $.comment,
  ],

  word: $ => $.identifier,

  conflicts: $ => [
    [$.type_specifier, $._expression],
  ],

  rules: {
    translation_unit: $ => repeat($._top_level_item),

    _top_level_item: $ => choice(
      $.preproc_include,
      $.preproc_directive,
      $.component_definition,
      $.component_declaration,
      $.typedef_declaration,
      $.struct_declaration,
      $.function_definition,
      $.declaration,
    ),

    /* ── Preprocessor & Comments ──────────────────────────────────────── */

    preproc_include: $ => seq(
      '#include',
      field('path', choice($.system_lib_string, $.string_literal)),
      /\r?\n/,
    ),

    system_lib_string: _ => /<[^>\n]+>/,

    preproc_directive: _ => /#[ \t]*(define|undef|if|ifdef|ifndef|elif|else|endif|pragma|error|line)[^\n]*\r?\n?/,

    comment: _ => token(choice(
      seq('//', /[^\n]*/),
      seq('/*', /[^*]*\*+([^/*][^*]*\*+)*/, '/'),
    )),

    /* ── `.cx` Component Declarations ─────────────────────────────────── */

    component_definition: $ => seq(
      optional($.storage_class_specifier),
      'component',
      field('name', $.component_identifier),
      field('parameters', $.parameter_list),
      field('body', $.compound_statement),
    ),

    component_declaration: $ => seq(
      optional($.storage_class_specifier),
      'component',
      field('name', $.component_identifier),
      field('parameters', $.parameter_list),
      ';',
    ),

    component_identifier: _ => /[A-Z][a-zA-Z0-9_]*/,

    /* ── C Declarations & Functions ───────────────────────────────────── */

    typedef_declaration: $ => seq(
      'typedef',
      $._type_prefix,
      optional($.declarator),
      ';',
    ),

    struct_declaration: $ => seq(
      $.struct_specifier,
      ';',
    ),

    function_definition: $ => seq(
      repeat($.storage_class_specifier),
      $._type_prefix,
      field('declarator', $.function_declarator),
      field('body', $.compound_statement),
    ),

    declaration: $ => seq(
      repeat($.storage_class_specifier),
      $._type_prefix,
      commaSep1($.init_declarator),
      ';',
    ),

    init_declarator: $ => seq(
      field('declarator', $.declarator),
      optional(seq('=', field('value', $._expression))),
    ),

    storage_class_specifier: _ => choice('static', 'extern', 'inline', 'register'),

    type_qualifier: _ => choice('const', 'volatile', 'restrict'),

    _type_prefix: $ => seq(
      repeat($.type_qualifier),
      $.type_specifier,
      repeat($.type_qualifier),
    ),

    type_specifier: $ => choice(
      $.primitive_type,
      $.struct_specifier,
      $.enum_specifier,
      $.identifier,
      $.component_identifier,
    ),

    primitive_type: _ => choice(
      'void', 'char', 'short', 'int', 'long', 'float', 'double',
      'signed', 'unsigned', 'bool', '_Bool', 'size_t', 'ssize_t',
      'int8_t', 'int16_t', 'int32_t', 'int64_t',
      'uint8_t', 'uint16_t', 'uint32_t', 'uint64_t', 'uintptr_t',
    ),

    struct_specifier: $ => prec.right(seq(
      choice('struct', 'union'),
      optional(field('name', choice($.identifier, $.component_identifier))),
      optional($.field_declaration_list),
    )),

    field_declaration_list: $ => seq(
      '{',
      repeat($.field_declaration),
      '}',
    ),

    field_declaration: $ => seq(
      $._type_prefix,
      commaSep1($.declarator),
      ';',
    ),

    enum_specifier: $ => prec.right(seq(
      'enum',
      optional(field('name', choice($.identifier, $.component_identifier))),
      optional(seq('{', commaSep($.enumerator), optional(','), '}')),
    )),

    enumerator: $ => seq(
      field('name', $.identifier),
      optional(seq('=', field('value', $._expression))),
    ),

    declarator: $ => choice(
      $.pointer_declarator,
      $.array_declarator,
      $.function_declarator,
      $.parenthesized_declarator,
      $.identifier,
      $.component_identifier,
    ),

    pointer_declarator: $ => prec.right(PREC.UNARY, seq(
      '*',
      repeat($.type_qualifier),
      $.declarator,
    )),

    array_declarator: $ => prec(PREC.CALL, seq(
      $.declarator,
      '[',
      optional($._expression),
      ']',
    )),

    parenthesized_declarator: $ => seq('(', $.declarator, ')'),

    function_declarator: $ => prec(PREC.CALL, seq(
      field('declarator', choice($.pointer_declarator, $.parenthesized_declarator, $.identifier, $.component_identifier)),
      field('parameters', $.parameter_list),
    )),

    parameter_list: $ => seq(
      '(',
      commaSep($.parameter_declaration),
      ')',
    ),

    parameter_declaration: $ => choice(
      '...',
      seq(
        $._type_prefix,
        optional($.declarator),
      ),
    ),

    type_name: $ => seq(
      $._type_prefix,
      repeat(seq('*', repeat($.type_qualifier))),
    ),

    /* ── Statements ───────────────────────────────────────────────────── */

    compound_statement: $ => seq(
      '{',
      repeat($._block_item),
      '}',
    ),

    _block_item: $ => choice(
      $.declaration,
      $._statement,
    ),

    _statement: $ => choice(
      $.compound_statement,
      $.return_statement,
      $.if_statement,
      $.for_statement,
      $.while_statement,
      $.break_statement,
      $.continue_statement,
      $.expression_statement,
      ';',
    ),

    return_statement: $ => seq('return', optional($._expression), ';'),

    if_statement: $ => prec.right(seq(
      'if',
      '(',
      field('condition', $._expression),
      ')',
      field('consequence', $._statement),
      optional(seq('else', field('alternative', $._statement))),
    )),

    for_statement: $ => seq(
      'for',
      '(',
      field('initializer', choice($.declaration, seq(optional($._expression), ';'))),
      field('condition', optional($._expression)),
      ';',
      field('update', optional($._expression)),
      ')',
      field('body', $._statement),
    ),

    while_statement: $ => seq(
      'while',
      '(',
      field('condition', $._expression),
      ')',
      field('body', $._statement),
    ),

    break_statement: _ => seq('break', ';'),
    continue_statement: _ => seq('continue', ';'),

    expression_statement: $ => seq($._expression, ';'),

    /* ── `.cx` JSX Grammar ────────────────────────────────────────────── */

    _jsx_node: $ => choice(
      $.jsx_element,
      $.jsx_self_closing_element,
      $.jsx_fragment,
    ),

    jsx_element: $ => prec(PREC.JSX, seq(
      $.jsx_opening_element,
      repeat($._jsx_child),
      $.jsx_closing_element,
    )),

    jsx_opening_element: $ => seq(
      '<',
      field('name', $._jsx_tag_name),
      repeat($.jsx_attribute),
      '>',
    ),

    jsx_closing_element: $ => seq(
      '</',
      field('name', $._jsx_tag_name),
      '>',
    ),

    jsx_self_closing_element: $ => prec(PREC.JSX, seq(
      '<',
      field('name', $._jsx_tag_name),
      repeat($.jsx_attribute),
      '/>',
    )),

    jsx_fragment: $ => prec(PREC.JSX, seq(
      '<>',
      repeat($._jsx_child),
      '</>',
    )),

    _jsx_tag_name: $ => choice(
      $.jsx_component_name,
      $.jsx_tag_name,
    ),

    jsx_component_name: _ => /[A-Z][a-zA-Z0-9_]*/,
    jsx_tag_name: _ => /[a-z][a-zA-Z0-9_-]*/,

    jsx_attribute: $ => seq(
      field('name', $.jsx_attribute_name),
      optional(seq(
        '=',
        field('value', choice(
          $.string_literal,
          $.jsx_expression,
        )),
      )),
    ),

    jsx_attribute_name: _ => /[a-zA-Z_][a-zA-Z0-9_:-]*/,

    _jsx_child: $ => choice(
      $._jsx_node,
      $.jsx_for_expression,
      $.jsx_if_expression,
      $.jsx_expression,
      $.jsx_text,
    ),

    jsx_text: _ => /[^\s{}<>]([^{}<>]*[^\s{}<>])?/,

    jsx_expression: $ => seq(
      '{',
      optional($._expression),
      '}',
    ),

    jsx_for_expression: $ => seq(
      '{',
      'for',
      '(',
      field('initializer', choice($.declaration, seq(optional($._expression), ';'))),
      field('condition', optional($._expression)),
      ';',
      field('update', optional($._expression)),
      ')',
      field('body', choice($._jsx_node, $._statement)),
      '}',
    ),

    jsx_if_expression: $ => seq(
      '{',
      'if',
      '(',
      field('condition', $._expression),
      ')',
      field('consequence', choice($._jsx_node, $._statement)),
      optional(seq(
        'else',
        field('alternative', choice($._jsx_node, $._statement)),
      )),
      '}',
    ),

    /* ── `.cx` Portable C99 Closures ──────────────────────────────────── */

    closure_expression: $ => seq(
      field('captures', $.capture_list),
      field('parameters', $.parameter_list),
      field('body', $.compound_statement),
    ),

    capture_list: $ => seq(
      '[',
      commaSep($.capture_item),
      ']',
    ),

    capture_item: $ => choice(
      seq(
        $._type_prefix,
        $.declarator,
        optional(seq('=', field('value', $._expression))),
      ),
      seq(
        field('name', $.identifier),
        optional(seq('=', field('value', $._expression))),
      ),
    ),

    /* ── Expressions ──────────────────────────────────────────────────── */

    _expression: $ => choice(
      $._jsx_node,
      $.closure_expression,
      $.assignment_expression,
      $.conditional_expression,
      $.binary_expression,
      $.unary_expression,
      $.update_expression,
      $.cast_expression,
      $.sizeof_expression,
      $.call_expression,
      $.field_expression,
      $.subscript_expression,
      $.parenthesized_expression,
      $.identifier,
      $.component_identifier,
      $.number_literal,
      $.string_literal,
      $.char_literal,
      $.null_literal,
      $.true_literal,
      $.false_literal,
    ),

    parenthesized_expression: $ => seq('(', $._expression, ')'),

    assignment_expression: $ => prec.right(PREC.ASSIGN, seq(
      field('left', $._expression),
      field('operator', choice('=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<=', '>>=')),
      field('right', $._expression),
    )),

    conditional_expression: $ => prec.right(PREC.TERNARY, seq(
      field('condition', $._expression),
      '?',
      field('consequence', $._expression),
      ':',
      field('alternative', $._expression),
    )),

    binary_expression: $ => {
      const table = [
        [PREC.OR, '||'],
        [PREC.AND, '&&'],
        [PREC.BITOR, '|'],
        [PREC.BITXOR, '^'],
        [PREC.BITAND, '&'],
        [PREC.EQ, choice('==', '!=')],
        [PREC.REL, choice('<', '>', '<=', '>=')],
        [PREC.SHIFT, choice('<<', '>>')],
        [PREC.ADD, choice('+', '-')],
        [PREC.MUL, choice('*', '/', '%')],
      ];
      return choice(...table.map(([p, op]) =>
        prec.left(p, seq(field('left', $._expression), field('operator', op), field('right', $._expression)))
      ));
    },

    unary_expression: $ => prec.right(PREC.UNARY, seq(
      field('operator', choice('!', '~', '-', '+', '*', '&')),
      field('argument', $._expression),
    )),

    update_expression: $ => choice(
      prec.right(PREC.UNARY, seq(choice('++', '--'), $._expression)),
      prec.left(PREC.CALL, seq($._expression, choice('++', '--'))),
    ),

    cast_expression: $ => prec.right(PREC.CAST, seq(
      '(',
      field('type', $.type_name),
      ')',
      field('value', $._expression),
    )),

    sizeof_expression: $ => prec.right(PREC.UNARY, seq(
      'sizeof',
      choice(
        seq('(', $.type_name, ')'),
        $._expression,
      ),
    )),

    call_expression: $ => prec(PREC.CALL, seq(
      field('function', $._expression),
      field('arguments', $.argument_list),
    )),

    argument_list: $ => seq(
      '(',
      commaSep(choice($._expression, prec.dynamic(-1, $.type_name))),
      ')',
    ),

    field_expression: $ => prec.left(PREC.MEMBER, seq(
      field('argument', $._expression),
      field('operator', choice('.', '->')),
      field('field', $.identifier),
    )),

    subscript_expression: $ => prec(PREC.CALL, seq(
      field('argument', $._expression),
      '[',
      field('index', $._expression),
      ']',
    )),

    /* ── Literals & Identifiers ───────────────────────────────────────── */

    identifier: _ => /[a-z_][a-zA-Z0-9_]*/,
    number_literal: _ => /(0[xX][0-9a-fA-F]+|[0-9]+(\.[0-9]+)?([eE][+-]?[0-9]+)?)[uUlLfF]*/,
    string_literal: _ => /"([^"\\\n]|\\.)*"/,
    char_literal: _ => /'([^'\\\n]|\\.)*'/,
    null_literal: _ => 'NULL',
    true_literal: _ => 'true',
    false_literal: _ => 'false',
  },
});

function commaSep(rule) {
  return optional(commaSep1(rule));
}

function commaSep1(rule) {
  return seq(rule, repeat(seq(',', rule)));
}
