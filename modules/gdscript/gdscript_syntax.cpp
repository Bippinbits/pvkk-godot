/**************************************************************************/
/*  gdscript_syntax.cpp                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "gdscript_syntax.h"

#include "gdscript_parser.h"

namespace {

class GDScriptSyntaxTreeBuilder {
	using Parser = GDScriptParser;

	Array *calls = nullptr;
	Array strings;

	static String node_kind(Parser::Node::Type p_type) {
		switch (p_type) {
			case Parser::Node::NONE:
				return "none";
			case Parser::Node::ANNOTATION:
				return "annotation";
			case Parser::Node::ARRAY:
				return "array";
			case Parser::Node::ASSERT:
				return "assert";
			case Parser::Node::ASSIGNMENT:
				return "assignment";
			case Parser::Node::AWAIT:
				return "await";
			case Parser::Node::BINARY_OPERATOR:
				return "binary_op";
			case Parser::Node::BREAK:
				return "break";
			case Parser::Node::BREAKPOINT:
				return "breakpoint";
			case Parser::Node::CALL:
				return "call";
			case Parser::Node::CAST:
				return "cast";
			case Parser::Node::CLASS:
				return "class";
			case Parser::Node::CONSTANT:
				return "constant";
			case Parser::Node::CONTINUE:
				return "continue";
			case Parser::Node::DICTIONARY:
				return "dictionary";
			case Parser::Node::ENUM:
				return "enum";
			case Parser::Node::FOR:
				return "for";
			case Parser::Node::FUNCTION:
				return "function";
			case Parser::Node::GET_NODE:
				return "get_node";
			case Parser::Node::IDENTIFIER:
				return "identifier";
			case Parser::Node::IF:
				return "if";
			case Parser::Node::LAMBDA:
				return "lambda";
			case Parser::Node::LITERAL:
				return "literal";
			case Parser::Node::MATCH:
				return "match";
			case Parser::Node::MATCH_BRANCH:
				return "match_branch";
			case Parser::Node::PARAMETER:
				return "parameter";
			case Parser::Node::PASS:
				return "pass";
			case Parser::Node::PATTERN:
				return "pattern";
			case Parser::Node::PRELOAD:
				return "preload";
			case Parser::Node::RETURN:
				return "return";
			case Parser::Node::SELF:
				return "self";
			case Parser::Node::SIGNAL:
				return "signal";
			case Parser::Node::SUBSCRIPT:
				return "subscript";
			case Parser::Node::SUITE:
				return "suite";
			case Parser::Node::TERNARY_OPERATOR:
				return "ternary_op";
			case Parser::Node::TYPE:
				return "type";
			case Parser::Node::TYPE_TEST:
				return "type_test";
			case Parser::Node::UNARY_OPERATOR:
				return "unary_op";
			case Parser::Node::VARIABLE:
				return "variable";
			case Parser::Node::WHILE:
				return "while";
		}
		return "unknown";
	}

	static String identifier_name(const Parser::IdentifierNode *p_identifier) {
		return p_identifier != nullptr ? String(p_identifier->name) : String();
	}

	static Dictionary make_node(const Parser::Node *p_node, const String &p_kind) {
		Dictionary node;
		node["kind"] = p_kind;
		node["line"] = p_node->start_line;
		node["column"] = p_node->start_column;
		return node;
	}

	static Dictionary make_node(const Parser::Node *p_node) {
		return make_node(p_node, node_kind(p_node->type));
	}

	static String type_text(const Parser::TypeNode *p_type) {
		if (p_type == nullptr) {
			return String();
		}
		PackedStringArray chain;
		for (const Parser::IdentifierNode *identifier : p_type->type_chain) {
			chain.push_back(identifier_name(identifier));
		}
		String text = String(".").join(chain);
		if (!p_type->container_types.is_empty()) {
			PackedStringArray containers;
			for (const Parser::TypeNode *container : p_type->container_types) {
				containers.push_back(type_text(container));
			}
			text += "[" + String(", ").join(containers) + "]";
		}
		return text;
	}

	static bool has_value(const Variant &p_expression) {
		return p_expression.get_type() == Variant::DICTIONARY && Dictionary(p_expression).has("value");
	}

	static bool has_annotation(const Parser::Node *p_node, const String &p_prefix, bool p_exact) {
		for (const Parser::AnnotationNode *annotation : p_node->annotations) {
			const String name = annotation->name;
			if (p_exact ? name == p_prefix : name.begins_with(p_prefix)) {
				return true;
			}
		}
		return false;
	}

	Array expressions(const Vector<Parser::ExpressionNode *> &p_expressions) {
		Array result;
		for (const Parser::ExpressionNode *expression : p_expressions) {
			result.push_back(build_expression(expression));
		}
		return result;
	}

	static bool all_have_values(const Array &p_expressions) {
		for (const Variant &expression : p_expressions) {
			if (!has_value(expression)) {
				return false;
			}
		}
		return true;
	}

	Dictionary build_annotation(const Parser::AnnotationNode *p_annotation) {
		Dictionary annotation = make_node(p_annotation);
		annotation["name"] = p_annotation->name;
		annotation["args"] = expressions(p_annotation->arguments);
		return annotation;
	}

	Array build_annotations(const Parser::Node *p_node) {
		Array annotations;
		for (const Parser::AnnotationNode *annotation : p_node->annotations) {
			annotations.push_back(build_annotation(annotation));
		}
		return annotations;
	}

	Variant build_expression(const Parser::ExpressionNode *p_expression) {
		if (p_expression == nullptr) {
			return Variant();
		}
		Dictionary node = make_node(p_expression);
		switch (p_expression->type) {
			case Parser::Node::LITERAL: {
				const Variant &value = static_cast<const Parser::LiteralNode *>(p_expression)->value;
				node["value"] = value;
				if (value.get_type() == Variant::STRING || value.get_type() == Variant::STRING_NAME) {
					strings.push_back(String(value));
				}
			} break;
			case Parser::Node::IDENTIFIER: {
				const String name = identifier_name(static_cast<const Parser::IdentifierNode *>(p_expression));
				node["name"] = name;
				node["path"] = name;
			} break;
			case Parser::Node::SELF: {
				node["path"] = "self";
			} break;
			case Parser::Node::SUBSCRIPT: {
				const Parser::SubscriptNode *subscript = static_cast<const Parser::SubscriptNode *>(p_expression);
				Variant base = build_expression(subscript->base);
				node["base"] = base;
				if (subscript->is_attribute) {
					node["kind"] = "attribute";
					const String name = identifier_name(subscript->attribute);
					node["name"] = name;
					if (base.get_type() == Variant::DICTIONARY && Dictionary(base).has("path") && String(Dictionary(base)["kind"]) != "call") {
						node["path"] = String(Dictionary(base)["path"]) + "." + name;
					}
				} else {
					node["index"] = build_expression(subscript->index);
				}
			} break;
			case Parser::Node::CALL: {
				const Parser::CallNode *call = static_cast<const Parser::CallNode *>(p_expression);
				if (calls != nullptr) {
					calls->push_back(node);
				}
				node["name"] = call->function_name;
				node["super"] = call->is_super;
				Variant callee = build_expression(call->callee);
				node["callee"] = callee;
				if (callee.get_type() == Variant::DICTIONARY) {
					Dictionary callee_node = callee;
					if (callee_node.has("path")) {
						node["path"] = callee_node["path"];
					}
					if (String(callee_node["kind"]) == "attribute") {
						node["base"] = callee_node["base"];
					}
				}
				node["args"] = expressions(call->arguments);
			} break;
			case Parser::Node::ARRAY: {
				Array elements = expressions(static_cast<const Parser::ArrayNode *>(p_expression)->elements);
				node["elements"] = elements;
				if (all_have_values(elements)) {
					Array value;
					for (const Variant &element : elements) {
						value.push_back(Dictionary(element)["value"]);
					}
					node["value"] = value;
				}
			} break;
			case Parser::Node::DICTIONARY: {
				const Parser::DictionaryNode *dictionary = static_cast<const Parser::DictionaryNode *>(p_expression);
				const bool is_lua = dictionary->style == Parser::DictionaryNode::LUA_TABLE;
				node["style"] = is_lua ? "lua" : "python";
				Array elements;
				Dictionary value;
				bool constant = true;
				for (const Parser::DictionaryNode::Pair &pair : dictionary->elements) {
					Dictionary element;
					Variant key = build_expression(pair.key);
					Variant element_value = build_expression(pair.value);
					element["key"] = key;
					element["value"] = element_value;
					elements.push_back(element);
					if (!constant) {
						continue;
					}
					Variant key_value;
					if (is_lua && pair.key != nullptr && pair.key->type == Parser::Node::IDENTIFIER) {
						key_value = static_cast<const Parser::IdentifierNode *>(pair.key)->name;
					} else if (has_value(key)) {
						key_value = Dictionary(key)["value"];
					} else {
						constant = false;
						continue;
					}
					if (!has_value(element_value)) {
						constant = false;
						continue;
					}
					value[key_value] = Dictionary(element_value)["value"];
				}
				node["elements"] = elements;
				if (constant) {
					node["value"] = value;
				}
			} break;
			case Parser::Node::UNARY_OPERATOR: {
				const Parser::UnaryOpNode *unary = static_cast<const Parser::UnaryOpNode *>(p_expression);
				switch (unary->operation) {
					case Parser::UnaryOpNode::OP_POSITIVE:
						node["op"] = "+";
						break;
					case Parser::UnaryOpNode::OP_NEGATIVE:
						node["op"] = "-";
						break;
					case Parser::UnaryOpNode::OP_COMPLEMENT:
						node["op"] = "~";
						break;
					case Parser::UnaryOpNode::OP_LOGIC_NOT:
						node["op"] = "not";
						break;
				}
				Variant operand = build_expression(unary->operand);
				node["operand"] = operand;
				if (has_value(operand) && unary->variant_op != Variant::OP_MAX) {
					bool valid = false;
					Variant result;
					Variant::evaluate(unary->variant_op, Dictionary(operand)["value"], Variant(), result, valid);
					if (valid) {
						node["value"] = result;
					}
				}
			} break;
			case Parser::Node::BINARY_OPERATOR: {
				const Parser::BinaryOpNode *binary = static_cast<const Parser::BinaryOpNode *>(p_expression);
				node["op"] = Variant::get_operator_name(binary->variant_op);
				Variant left = build_expression(binary->left_operand);
				Variant right = build_expression(binary->right_operand);
				node["left"] = left;
				node["right"] = right;
				if (has_value(left) && has_value(right) && binary->variant_op != Variant::OP_MAX) {
					bool valid = false;
					Variant result;
					Variant::evaluate(binary->variant_op, Dictionary(left)["value"], Dictionary(right)["value"], result, valid);
					if (valid) {
						node["value"] = result;
					}
				}
			} break;
			case Parser::Node::TERNARY_OPERATOR: {
				const Parser::TernaryOpNode *ternary = static_cast<const Parser::TernaryOpNode *>(p_expression);
				node["condition"] = build_expression(ternary->condition);
				node["true_expr"] = build_expression(ternary->true_expr);
				node["false_expr"] = build_expression(ternary->false_expr);
			} break;
			case Parser::Node::ASSIGNMENT: {
				const Parser::AssignmentNode *assignment = static_cast<const Parser::AssignmentNode *>(p_expression);
				node["op"] = assignment->variant_op == Variant::OP_MAX ? String("=") : Variant::get_operator_name(assignment->variant_op) + "=";
				node["target"] = build_expression(assignment->assignee);
				node["assigned"] = build_expression(assignment->assigned_value);
			} break;
			case Parser::Node::AWAIT: {
				node["operand"] = build_expression(static_cast<const Parser::AwaitNode *>(p_expression)->to_await);
			} break;
			case Parser::Node::CAST: {
				const Parser::CastNode *cast = static_cast<const Parser::CastNode *>(p_expression);
				node["operand"] = build_expression(cast->operand);
				node["type"] = type_text(cast->cast_type);
			} break;
			case Parser::Node::TYPE_TEST: {
				const Parser::TypeTestNode *type_test = static_cast<const Parser::TypeTestNode *>(p_expression);
				node["operand"] = build_expression(type_test->operand);
				node["type"] = type_text(type_test->test_type);
			} break;
			case Parser::Node::PRELOAD: {
				node["path"] = build_expression(static_cast<const Parser::PreloadNode *>(p_expression)->path);
			} break;
			case Parser::Node::GET_NODE: {
				const Parser::GetNodeNode *get_node = static_cast<const Parser::GetNodeNode *>(p_expression);
				node["node_path"] = get_node->full_path;
				node["use_dollar"] = get_node->use_dollar;
			} break;
			case Parser::Node::LAMBDA: {
				const Parser::LambdaNode *lambda = static_cast<const Parser::LambdaNode *>(p_expression);
				if (lambda->function != nullptr) {
					node["function"] = build_function(lambda->function, false);
				}
			} break;
			default:
				break;
		}
		return node;
	}

	Array build_suite(const Parser::SuiteNode *p_suite) {
		Array statements;
		if (p_suite == nullptr) {
			return statements;
		}
		for (const Parser::Node *statement : p_suite->statements) {
			statements.push_back(build_statement(statement));
		}
		return statements;
	}

	Dictionary build_pattern(const Parser::PatternNode *p_pattern) {
		Dictionary node = make_node(p_pattern);
		switch (p_pattern->pattern_type) {
			case Parser::PatternNode::PT_LITERAL:
				node["pattern"] = "literal";
				node["expression"] = build_expression(p_pattern->literal);
				break;
			case Parser::PatternNode::PT_EXPRESSION:
				node["pattern"] = "expression";
				node["expression"] = build_expression(p_pattern->expression);
				break;
			case Parser::PatternNode::PT_BIND:
				node["pattern"] = "bind";
				node["name"] = identifier_name(p_pattern->bind);
				break;
			case Parser::PatternNode::PT_ARRAY: {
				node["pattern"] = "array";
				Array elements;
				for (const Parser::PatternNode *element : p_pattern->array) {
					if (element != nullptr) {
						elements.push_back(build_pattern(element));
					}
				}
				node["elements"] = elements;
			} break;
			case Parser::PatternNode::PT_DICTIONARY: {
				node["pattern"] = "dictionary";
				Array elements;
				for (const Parser::PatternNode::Pair &pair : p_pattern->dictionary) {
					Dictionary element;
					element["key"] = build_expression(pair.key);
					element["value"] = pair.value_pattern != nullptr ? Variant(build_pattern(pair.value_pattern)) : Variant();
					elements.push_back(element);
				}
				node["elements"] = elements;
			} break;
			case Parser::PatternNode::PT_REST:
				node["pattern"] = "rest";
				break;
			case Parser::PatternNode::PT_WILDCARD:
				node["pattern"] = "wildcard";
				break;
		}
		return node;
	}

	Variant build_statement(const Parser::Node *p_statement) {
		if (p_statement == nullptr) {
			return Variant();
		}
		if (p_statement->is_expression()) {
			Dictionary node = build_expression(static_cast<const Parser::ExpressionNode *>(p_statement));
			if (!p_statement->annotations.is_empty()) {
				node["annotations"] = build_annotations(p_statement);
			}
			return node;
		}
		Dictionary node = make_node(p_statement);
		if (!p_statement->annotations.is_empty()) {
			node["annotations"] = build_annotations(p_statement);
		}
		switch (p_statement->type) {
			case Parser::Node::VARIABLE:
			case Parser::Node::CONSTANT: {
				const Parser::AssignableNode *assignable = static_cast<const Parser::AssignableNode *>(p_statement);
				node["name"] = identifier_name(assignable->identifier);
				node["type"] = type_text(assignable->datatype_specifier);
				node["inferred"] = assignable->infer_datatype;
				node["initializer"] = build_expression(assignable->initializer);
			} break;
			case Parser::Node::IF: {
				const Parser::IfNode *if_node = static_cast<const Parser::IfNode *>(p_statement);
				node["condition"] = build_expression(if_node->condition);
				node["body"] = build_suite(if_node->true_block);
				node["else"] = if_node->false_block != nullptr ? Variant(build_suite(if_node->false_block)) : Variant();
			} break;
			case Parser::Node::FOR: {
				const Parser::ForNode *for_node = static_cast<const Parser::ForNode *>(p_statement);
				node["variable"] = identifier_name(for_node->variable);
				node["type"] = type_text(for_node->datatype_specifier);
				node["iterable"] = build_expression(for_node->list);
				node["body"] = build_suite(for_node->loop);
			} break;
			case Parser::Node::WHILE: {
				const Parser::WhileNode *while_node = static_cast<const Parser::WhileNode *>(p_statement);
				node["condition"] = build_expression(while_node->condition);
				node["body"] = build_suite(while_node->loop);
			} break;
			case Parser::Node::MATCH: {
				const Parser::MatchNode *match = static_cast<const Parser::MatchNode *>(p_statement);
				node["test"] = build_expression(match->test);
				Array branches;
				for (const Parser::MatchBranchNode *branch : match->branches) {
					if (branch == nullptr) {
						continue;
					}
					Dictionary branch_node = make_node(branch);
					Array patterns;
					for (const Parser::PatternNode *pattern : branch->patterns) {
						if (pattern != nullptr) {
							patterns.push_back(build_pattern(pattern));
						}
					}
					branch_node["patterns"] = patterns;
					branch_node["guard"] = branch->guard_body != nullptr ? Variant(build_suite(branch->guard_body)) : Variant();
					branch_node["body"] = build_suite(branch->block);
					if (!branch->annotations.is_empty()) {
						branch_node["annotations"] = build_annotations(branch);
					}
					branches.push_back(branch_node);
				}
				node["branches"] = branches;
			} break;
			case Parser::Node::RETURN: {
				node["value"] = build_expression(static_cast<const Parser::ReturnNode *>(p_statement)->return_value);
			} break;
			case Parser::Node::ASSERT: {
				const Parser::AssertNode *assert_node = static_cast<const Parser::AssertNode *>(p_statement);
				node["condition"] = build_expression(assert_node->condition);
				node["message"] = build_expression(assert_node->message);
			} break;
			case Parser::Node::SUITE: {
				node["body"] = build_suite(static_cast<const Parser::SuiteNode *>(p_statement));
			} break;
			default:
				break;
		}
		return node;
	}

	Dictionary build_parameter(const Parser::ParameterNode *p_parameter) {
		Dictionary node = make_node(p_parameter);
		node["name"] = identifier_name(p_parameter->identifier);
		node["type"] = type_text(p_parameter->datatype_specifier);
		node["inferred"] = p_parameter->infer_datatype;
		node["default"] = build_expression(p_parameter->initializer);
		return node;
	}

	Array build_parameters(const Vector<Parser::ParameterNode *> &p_parameters) {
		Array parameters;
		for (const Parser::ParameterNode *parameter : p_parameters) {
			if (parameter != nullptr) {
				parameters.push_back(build_parameter(parameter));
			}
		}
		return parameters;
	}

	Dictionary build_function(const Parser::FunctionNode *p_function, bool p_collect_calls) {
		Dictionary node = make_node(p_function);
		Array *previous_calls = calls;
		Array function_calls;
		if (p_collect_calls) {
			calls = &function_calls;
		}
		node["name"] = identifier_name(p_function->identifier);
		node["end_line"] = p_function->end_line;
		node["static"] = p_function->is_static;
		node["abstract"] = p_function->is_abstract || has_annotation(p_function, "@abstract", true);
		node["coroutine"] = p_function->is_coroutine;
		node["annotations"] = build_annotations(p_function);
		node["params"] = build_parameters(p_function->parameters);
		node["rest_param"] = p_function->rest_parameter != nullptr ? Variant(build_parameter(p_function->rest_parameter)) : Variant();
		node["return_type"] = type_text(p_function->return_type);
		node["body"] = build_suite(p_function->body);
		calls = previous_calls;
		if (p_collect_calls) {
			node["calls"] = function_calls;
		}
		return node;
	}

	Dictionary build_variable(const Parser::VariableNode *p_variable) {
		Dictionary node = make_node(p_variable);
		node["name"] = identifier_name(p_variable->identifier);
		node["type"] = type_text(p_variable->datatype_specifier);
		node["inferred"] = p_variable->infer_datatype;
		node["const"] = false;
		node["static"] = p_variable->is_static;
		node["onready"] = p_variable->onready || has_annotation(p_variable, "@onready", true);
		node["exported"] = p_variable->exported || has_annotation(p_variable, "@export", false);
		node["annotations"] = build_annotations(p_variable);
		node["initializer"] = build_expression(p_variable->initializer);
		switch (p_variable->property) {
			case Parser::VariableNode::PROP_NONE:
				node["property"] = "none";
				break;
			case Parser::VariableNode::PROP_INLINE:
				node["property"] = "inline";
				node["setter"] = p_variable->setter != nullptr ? Variant(build_function(p_variable->setter, true)) : Variant();
				node["getter"] = p_variable->getter != nullptr ? Variant(build_function(p_variable->getter, true)) : Variant();
				break;
			case Parser::VariableNode::PROP_SETGET:
				node["property"] = "setget";
				node["setter"] = identifier_name(p_variable->setter_pointer);
				node["getter"] = identifier_name(p_variable->getter_pointer);
				break;
		}
		return node;
	}

	Dictionary build_constant(const Parser::ConstantNode *p_constant) {
		Dictionary node = make_node(p_constant);
		node["name"] = identifier_name(p_constant->identifier);
		node["type"] = type_text(p_constant->datatype_specifier);
		node["inferred"] = p_constant->infer_datatype;
		node["const"] = true;
		node["static"] = false;
		node["onready"] = false;
		node["exported"] = false;
		node["annotations"] = build_annotations(p_constant);
		node["initializer"] = build_expression(p_constant->initializer);
		return node;
	}

	Dictionary build_enum_value(const Parser::EnumNode::Value &p_value) {
		Dictionary node;
		node["kind"] = "enum_value";
		node["line"] = p_value.line;
		node["column"] = p_value.start_column;
		node["name"] = identifier_name(p_value.identifier);
		node["value"] = build_expression(p_value.custom_value);
		return node;
	}

	Dictionary build_enum(const Parser::EnumNode *p_enum) {
		Dictionary node = make_node(p_enum);
		node["name"] = identifier_name(p_enum->identifier);
		node["annotations"] = build_annotations(p_enum);
		Array values;
		for (const Parser::EnumNode::Value &value : p_enum->values) {
			values.push_back(build_enum_value(value));
		}
		node["values"] = values;
		return node;
	}

	Dictionary build_signal(const Parser::SignalNode *p_signal) {
		Dictionary node = make_node(p_signal);
		node["name"] = identifier_name(p_signal->identifier);
		node["annotations"] = build_annotations(p_signal);
		node["params"] = build_parameters(p_signal->parameters);
		return node;
	}

public:
	Dictionary build_class(const Parser::ClassNode *p_class) {
		Dictionary node = make_node(p_class);
		Array *previous_calls = calls;
		Array class_calls;
		calls = &class_calls;

		node["name"] = identifier_name(p_class->identifier);
		node["end_line"] = p_class->end_line;
		node["extends_path"] = p_class->extends_path;
		PackedStringArray extends_chain;
		for (const Parser::IdentifierNode *identifier : p_class->extends) {
			extends_chain.push_back(identifier_name(identifier));
		}
		node["extends_class"] = String(".").join(extends_chain);
		node["abstract"] = p_class->is_abstract || has_annotation(p_class, "@abstract", true);
		node["icon_path"] = p_class->icon_path;
		node["annotations"] = build_annotations(p_class);

		Array members;
		Array functions;
		Array signals;
		Array enums;
		Array inner_classes;
		for (const Parser::ClassNode::Member &member : p_class->members) {
			switch (member.type) {
				case Parser::ClassNode::Member::VARIABLE:
					members.push_back(build_variable(member.variable));
					break;
				case Parser::ClassNode::Member::CONSTANT:
					members.push_back(build_constant(member.constant));
					break;
				case Parser::ClassNode::Member::GROUP: {
					Dictionary group = make_node(member.annotation, "group");
					group["annotation"] = build_annotation(member.annotation);
					members.push_back(group);
				} break;
				case Parser::ClassNode::Member::FUNCTION:
					functions.push_back(build_function(member.function, true));
					break;
				case Parser::ClassNode::Member::SIGNAL:
					signals.push_back(build_signal(member.signal));
					break;
				case Parser::ClassNode::Member::ENUM:
					enums.push_back(build_enum(member.m_enum));
					break;
				case Parser::ClassNode::Member::ENUM_VALUE:
					enums.push_back(build_enum_value(member.enum_value));
					break;
				case Parser::ClassNode::Member::CLASS:
					inner_classes.push_back(build_class(member.m_class));
					break;
				case Parser::ClassNode::Member::UNDEFINED:
					break;
			}
		}
		node["members"] = members;
		node["functions"] = functions;
		node["signals"] = signals;
		node["enums"] = enums;
		node["inner_classes"] = inner_classes;

		calls = previous_calls;
		node["calls"] = class_calls;
		return node;
	}

	Array get_strings() const {
		return strings;
	}
};

} // namespace

Dictionary GDScriptSyntax::parse(const String &p_source, const String &p_path) {
	GDScriptParser parser;
	const Error err = parser.parse(p_source, p_path, false);

	Array errors;
	for (const GDScriptParser::ParserError &error : parser.get_errors()) {
		Dictionary entry;
		entry["line"] = error.line;
		entry["column"] = error.column;
		entry["message"] = error.message;
		errors.push_back(entry);
	}

	Dictionary result;
	result["ok"] = err == OK && errors.is_empty();
	result["errors"] = errors;

	const GDScriptParser::ClassNode *tree = parser.get_tree();
	if (tree == nullptr) {
		result["class"] = Variant();
		result["strings"] = Array();
		return result;
	}

	GDScriptSyntaxTreeBuilder builder;
	Dictionary class_node = builder.build_class(tree);
	class_node["tool"] = parser.is_tool();
	result["class"] = class_node;
	result["strings"] = builder.get_strings();
	return result;
}

void GDScriptSyntax::_bind_methods() {
	ClassDB::bind_static_method("GDScriptSyntax", D_METHOD("parse", "source", "path"), &GDScriptSyntax::parse, DEFVAL(String()));
}
