// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <memory>
#include <string>
#include <sstream>
#include <vector>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <exception>
#include <type_traits>

#include <graphqlparser/Ast.h>
#include <graphqlparser/AstVisitor.h>

#include <cpprest/json.h>

namespace facebook {
namespace graphql {
namespace service {

// This exception bubbles up 1 or more error messages to the JSON results.
class schema_exception : public std::exception
{
public:
	schema_exception(const std::vector<std::string>& messages);

	const web::json::value& getErrors() const noexcept;

private:
	web::json::value _errors;
};

// Fragments are referenced by name and have a single type condition (except for inline
// fragments, where the type condition is common but optional). They contain a set of fields
// (with optional aliases and sub-selections) and potentially references to other fragments.
class Fragment
{
public:
	explicit Fragment(const ast::FragmentDefinition& fragmentDefinition);

	const std::string& getType() const;
	const ast::SelectionSet& getSelection() const;

private:
	std::string _type;

	const ast::SelectionSet& _selection;
};

// Resolvers for complex types need to be able to find fragment definitions anywhere in
// the request document by name.
using FragmentMap = std::unordered_map<std::string, Fragment>;

// Resolver functors take a set of arguments encoded as members on a JSON object
// with an optional selection set for complex types and return a JSON value for
// a single field.
struct ResolverParams
{
	const web::json::object& arguments;
	const ast::SelectionSet* selection;
	const FragmentMap& fragments;
	const web::json::object& variables;
};

using Resolver = std::function<web::json::value(ResolverParams&&)>;
using ResolverMap = std::unordered_map<std::string, Resolver>;

// Types be wrapped non-null or list types in GraphQL. Since nullability is a more special case
// in C++, we invert the default and apply that modifier instead when the non-null wrapper is
// not present in that part of the wrapper chain.
enum class TypeModifier
{
	None,
	Nullable,
	List,
};

struct DisableNone {};
struct DisableNullable {};
struct DisableList {};

// Extract individual arguments with chained type modifiers which add nullable or list wrappers.
// If the argument is not optional, use require and let it throw a schema_exception when the
// argument is missing or not the correct type. If it's nullable, use find and check the second
// element in the pair to see if it was found or if you just got the default value for that type.
template <typename _Type, TypeModifier _Modifier = TypeModifier::None, TypeModifier... _Other>
struct ModifiedArgument
{
	// Peel off modifiers until we get to the underlying type.
	using type = typename std::conditional<TypeModifier::Nullable == _Modifier,
		std::unique_ptr<typename ModifiedArgument<_Type, _Other...>::type>,
		typename std::conditional<TypeModifier::List == _Modifier,
			std::vector<typename ModifiedArgument<_Type, _Other...>::type>,
			_Type
		>::type
	>::type;

	// Peel off the none modifier. If it's included, it should always be last in the list.
	static type require(const typename std::conditional<TypeModifier::None == _Modifier, std::string, DisableNone>::type& name,
		const web::json::object& arguments)
	{
		static_assert(TypeModifier::None == _Modifier, "this is the empty version");
		static_assert(sizeof...(_Other) == 0, "TypeModifier::None should always be last in the list of modifiers if it's included at all");

		// Just call through to the partial specialization without the modifier.
		return ModifiedArgument<_Type>::require(std::move(name), arguments);
	}

	// Peel off nullable modifiers.
	static type require(const typename std::conditional<TypeModifier::Nullable == _Modifier, std::string, DisableNullable>::type& name,
		const web::json::object& arguments)
	{
		static_assert(TypeModifier::Nullable == _Modifier, "this is the nullable version");

		try
		{
			const auto& valueItr = arguments.find(utility::conversions::to_string_t(name));

			if (valueItr == arguments.cend()
				|| valueItr->second.is_null())
			{
				return std::unique_ptr<typename ModifiedArgument<_Type, _Other...>::type>();
			}

			auto result = ModifiedArgument<_Type, _Other...>::require(std::move(name), arguments);
			
			return std::unique_ptr<typename ModifiedArgument<_Type, _Other...>::type>(new decltype(result)(std::move(result)));
		}
		catch (const web::json::json_exception& ex)
		{
			std::ostringstream error;

			error << "Invalid argument: " << name << " message: " << ex.what();
			throw schema_exception({ error.str() });
		}
	}

	// Peel off list modifiers.
	static type require(const typename std::conditional<TypeModifier::List == _Modifier, std::string, DisableList>::type& name,
		const web::json::object& arguments)
	{
		static_assert(TypeModifier::List == _Modifier, "this is the list version");

		try
		{
			const auto& values = arguments.at(utility::conversions::to_string_t(name)).as_array();
			type result(values.size());

			std::transform(values.cbegin(), values.cend(), result.begin(),
				[](const web::json::value& element)
			{
				auto single = web::json::value::object({
					{ _XPLATSTR("value"), element }
				});

				return ModifiedArgument<_Type, _Other...>::require("value", single.as_object());
			});

			return result;
		}
		catch (const web::json::json_exception& ex)
		{
			std::ostringstream error;

			error << "Invalid argument: " << name << " message: " << ex.what();
			throw schema_exception({ error.str() });
		}
	}

	static std::pair<type, bool> find(const std::string& name, const web::json::object& arguments) noexcept
	{
		try
		{
			return { require(std::move(name), arguments), true };
		}
		catch (const schema_exception&)
		{
			type value;

			return { std::move(value), false };
		}
	}
};

// Handle the empty modifier list case.
template <typename _Type>
struct ModifiedArgument<_Type, TypeModifier::None>
{
	using type = _Type;

	// Call convert on this type without any other modifiers.
	static _Type require(const std::string& name, const web::json::object& arguments)
	{
		try
		{
			return convert(arguments.at(utility::conversions::to_string_t(name)));
		}
		catch (const web::json::json_exception& ex)
		{
			std::ostringstream error;

			error << "Invalid argument: " << name << " message: " << ex.what();
			throw schema_exception({ error.str() });
		}
	}

	// Wrap require in a try/catch block.
	static std::pair<_Type, bool> find(const std::string& name, const web::json::object& arguments) noexcept
	{
		try
		{
			return { require(std::move(name), arguments), true };
		}
		catch (const schema_exception&)
		{
			type value;

			return { std::move(value), false };
		}
	}

	// Convert a single value to the specified type.
	static _Type convert(const web::json::value& value);
};

// Convenient type aliases for testing, generated code won't actually use these. These are also
// the specializations which are implemented in the GraphQLService library, other specializations
// for input types should be generated in schemagen.
template <TypeModifier... _Modifiers> using IntArgument = ModifiedArgument<int, _Modifiers...>;
template <TypeModifier... _Modifiers> using FloatArgument = ModifiedArgument<double, _Modifiers...>;
template <TypeModifier... _Modifiers> using StringArgument = ModifiedArgument<std::string, _Modifiers...>;
template <TypeModifier... _Modifiers> using BooleanArgument = ModifiedArgument<bool, _Modifiers...>;
template <TypeModifier... _Modifiers> using IdArgument = ModifiedArgument<std::vector<unsigned char>, _Modifiers...>;
template <TypeModifier... _Modifiers> using ScalarArgument = ModifiedArgument<web::json::value, _Modifiers...>;

// Each type should handle fragments with type conditions matching its own
// name and any inheritted interfaces.
using TypeNames = std::unordered_set<std::string>;

// Object parses argument values, performs variable lookups, expands fragments, evaluates @include
// and @skip directives, and calls through to the resolver functor for each selected field with
// its arguments. This may be a recursive process for fields which return another complex type,
// in which case it requires its own selection set.
class Object : public std::enable_shared_from_this<Object>
{
public:
	explicit Object(TypeNames&& typeNames, ResolverMap&& resolvers);

	web::json::value resolve(const ast::SelectionSet& selection, const FragmentMap& fragments, const web::json::object& variables) const;

private:
	TypeNames _typeNames;
	ResolverMap _resolvers;
};

using TypeMap = std::unordered_map<std::string, std::shared_ptr<Object>>;

struct DisableNullableSharedPtr {};

// Convert the result of a resolver function with chained type modifiers that add nullable or
// list wrappers. This is the inverse of ModifiedArgument for output types instead of input types.
template <typename _Type, TypeModifier _Modifier = TypeModifier::None, TypeModifier... _Other>
struct ModifiedResult
{
	// Peel off modifiers until we get to the underlying type.
	using type = typename std::conditional<TypeModifier::Nullable == _Modifier,
		typename std::conditional<std::is_base_of<Object, _Type>::value
			&& std::is_same<std::shared_ptr<_Type>, typename ModifiedResult<_Type, _Other...>::type>::value,
			std::shared_ptr<_Type>,
			std::unique_ptr<typename ModifiedResult<_Type, _Other...>::type>
		>::type,
		typename std::conditional<TypeModifier::List == _Modifier,
			std::vector<typename ModifiedResult<_Type, _Other...>::type>,
			typename std::conditional<std::is_base_of<Object, _Type>::value,
				std::shared_ptr<_Type>,
				_Type>::type
		>::type
	>::type;

	// Peel off the none modifier. If it's included, it should always be last in the list.
	static web::json::value convert(const typename std::conditional<TypeModifier::None == _Modifier, type, DisableNone>::type& result,
		ResolverParams&& params)
	{
		static_assert(TypeModifier::None == _Modifier, "this is the empty version");
		static_assert(sizeof...(_Other) == 0, "TypeModifier::None should always be last in the list of modifiers if it's included at all");

		// Just call through to the partial specialization without the modifier.
		return ModifiedResult<_Type>::convert(result, std::move(params));
	}

	// Peel off nullable modifiers for std::shared_ptr<Object>.
	static web::json::value convert(const typename std::conditional<TypeModifier::Nullable == _Modifier
			&& std::is_same<std::shared_ptr<_Type>, typename ModifiedResult<_Type, _Other...>::type>::value, type, DisableNullableSharedPtr>::type& result,
		ResolverParams&& params)
	{
		static_assert(TypeModifier::Nullable == _Modifier, "this is the nullable version");
		static_assert(std::is_same<std::shared_ptr<_Type>, typename ModifiedResult<_Type, _Other...>::type>::value, "this is the shared_ptr version");

		if (!result)
		{
			return web::json::value::null();
		}

		return ModifiedResult<_Type, _Other...>::convert(result, std::move(params));
	}

	// Peel off nullable modifiers for anything else, which should all be std::unique_ptr.
	static web::json::value convert(const typename std::conditional<TypeModifier::Nullable == _Modifier
			&& !std::is_same<std::shared_ptr<_Type>, typename ModifiedResult<_Type, _Other...>::type>::value, type, DisableNullable>::type& result,
		ResolverParams&& params)
	{
		static_assert(TypeModifier::Nullable == _Modifier, "this is the nullable version");
		static_assert(!std::is_same<std::shared_ptr<_Type>, typename ModifiedResult<_Type, _Other...>::type>::value, "this is the unique_ptr version");

		if (!result)
		{
			return web::json::value::null();
		}

		return ModifiedResult<_Type, _Other...>::convert(*result, std::move(params));
	}

	// Peel off list modifiers.
	static web::json::value convert(const typename std::conditional<TypeModifier::List == _Modifier, type, DisableList>::type& result,
		ResolverParams&& params)
	{
		static_assert(TypeModifier::List == _Modifier, "this is the list version");

		auto value = web::json::value::array(result.size());

		std::transform(result.cbegin(), result.cend(), value.as_array().begin(),
			[params](const typename ModifiedResult<_Type, _Other...>::type& element)
		{
			return ModifiedResult<_Type, _Other...>::convert(element, ResolverParams(params));
		});

		return value;
	}
};

// Handle the empty modifier list case.
template <typename _Type>
struct ModifiedResult<_Type, TypeModifier::None>
{
	using type = typename std::conditional<std::is_base_of<Object, _Type>::value,
		std::shared_ptr<_Type>,
		_Type>::type;

	// Convert a subclass of Object and call that specialization.
	static web::json::value convert(const typename std::conditional<!std::is_same<Object, _Type>::value && std::is_base_of<Object, _Type>::value,
		type, DisableNone>::type& result, ResolverParams&& params)
	{
		static_assert(std::is_same<std::shared_ptr<_Type>, type>::value, "this is the derived object type");

		return ModifiedResult<Object>::convert(std::static_pointer_cast<Object>(result), std::move(params));
	}

	// Convert a single value of the specified type to JSON.
	static web::json::value convert(const typename std::conditional<!std::is_same<Object, _Type>::value && std::is_base_of<Object, _Type>::value,
		DisableNone, type>::type& result, ResolverParams&& params);
};

// Convenient type aliases for testing, generated code won't actually use these. These are also
// the specializations which are implemented in the GraphQLService library, other specializations
// for output types should be generated in schemagen.
template <TypeModifier... _Modifiers> using IntResult = ModifiedResult<int, _Modifiers...>;
template <TypeModifier... _Modifiers> using FloatResult = ModifiedResult<double, _Modifiers...>;
template <TypeModifier... _Modifiers> using StringResult = ModifiedResult<std::string, _Modifiers...>;
template <TypeModifier... _Modifiers> using BooleanResult = ModifiedResult<bool, _Modifiers...>;
template <TypeModifier... _Modifiers> using IdResult = ModifiedResult<std::vector<unsigned char>, _Modifiers...>;
template <TypeModifier... _Modifiers> using ScalarResult = ModifiedResult<web::json::value, _Modifiers...>;
template <TypeModifier... _Modifiers> using ObjectResult = ModifiedResult<Object, _Modifiers...>;

// Request scans the fragment definitions and finds the right operation definition to interpret
// depending on the operation name (which might be empty for a single-operation document). It
// also needs the values of hte request variables.
class Request : public std::enable_shared_from_this<Request>
{
public:
	explicit Request(TypeMap&& operationTypes);

	web::json::value resolve(const ast::Node& document, const std::string& operationName, const web::json::object& variables) const;

private:
	TypeMap _operations;
};

// SelectionVisitor visits the AST and resolves a field or fragment, unless its skipped by
// a directive or type condition.
class SelectionVisitor : public ast::visitor::AstVisitor
{
public:
	SelectionVisitor(const FragmentMap& fragments, const web::json::object& variables, const TypeNames& typeNames, const ResolverMap& resolvers);

	web::json::value getValues();

	bool visitField(const ast::Field& field) override;
	bool visitFragmentSpread(const ast::FragmentSpread &fragmentSpread) override;
	bool visitInlineFragment(const ast::InlineFragment &inlineFragment) override;

private:
	bool shouldSkip(const std::vector<std::unique_ptr<ast::Directive>>* directives) const;

	const FragmentMap& _fragments;
	const web::json::object& _variables;
	const TypeNames& _typeNames;
	const ResolverMap& _resolvers;
	web::json::value _values;
};

// ValueVisitor visits the AST and builds a JSON representation of any value
// hardcoded or referencing a variable in an operation.
class ValueVisitor : public ast::visitor::AstVisitor
{
public:
	ValueVisitor(const web::json::object& variables);

	web::json::value getValue();

	bool visitVariable(const ast::Variable& variable) override;
	bool visitIntValue(const ast::IntValue& intValue) override;
	bool visitFloatValue(const ast::FloatValue& floatValue) override;
	bool visitStringValue(const ast::StringValue& stringValue) override;
	bool visitBooleanValue(const ast::BooleanValue& booleanValue) override;
	bool visitNullValue(const ast::NullValue& nullValue) override;
	bool visitEnumValue(const ast::EnumValue& enumValue) override;
	bool visitListValue(const ast::ListValue& listValue) override;
	bool visitObjectValue(const ast::ObjectValue& objectValue) override;

private:
	const web::json::object& _variables;
	web::json::value _value;
};

// FragmentDefinitionVisitor visits the AST and collects all of the fragment
// definitions in the document.
class FragmentDefinitionVisitor : public ast::visitor::AstVisitor
{
public:
	FragmentDefinitionVisitor();

	FragmentMap getFragments();

	bool visitFragmentDefinition(const ast::FragmentDefinition& fragmentDefinition) override;

private:
	FragmentMap _fragments;
};

// OperationDefinitionVisitor visits the AST and executes the one with the specified
// operation name.
class OperationDefinitionVisitor : public ast::visitor::AstVisitor
{
public:
	OperationDefinitionVisitor(const TypeMap& operations, const std::string& operationName, const web::json::object& variables, const FragmentMap& fragments);

	web::json::value getValue();

	bool visitOperationDefinition(const ast::OperationDefinition& operationDefinition) override;

private:
	const TypeMap& _operations;
	const std::string& _operationName;
	const web::json::object& _variables;
	const FragmentMap& _fragments;
	web::json::value _result;
};

} /* namespace service */
} /* namespace graphql */
} /* namespace facebook */