
#include <libsolidity/InterfaceHandler.h>
#include <boost/range/irange.hpp>
#include <libsolidity/AST.h>
#include <libsolidity/CompilerStack.h>
using namespace std;

namespace dev
{
namespace solidity
{

/* -- public -- */

InterfaceHandler::InterfaceHandler()
{
	m_lastTag = DocTagType::None;
}

string InterfaceHandler::documentation(
	ContractDefinition const& _contractDef,
	DocumentationType _type
)
{
	switch(_type)
	{
	case DocumentationType::NatspecUser:
		return userDocumentation(_contractDef);
	case DocumentationType::NatspecDev:
		return devDocumentation(_contractDef);
	case DocumentationType::ABIInterface:
		return abiInterface(_contractDef);
	case DocumentationType::ABISolidityInterface:
		return ABISolidityInterface(_contractDef);
	}

	BOOST_THROW_EXCEPTION(InternalCompilerError() << errinfo_comment("Unknown documentation type"));
	return "";
}

string InterfaceHandler::abiInterface(ContractDefinition const& _contractDef)
{
	Json::Value abi(Json::arrayValue);

	auto populateParameters = [](vector<string> const& _paramNames, vector<string> const& _paramTypes)
	{
		Json::Value params(Json::arrayValue);
		solAssert(_paramNames.size() == _paramTypes.size(), "Names and types vector size does not match");
		for (unsigned i = 0; i < _paramNames.size(); ++i)
		{
			Json::Value param;
			param["name"] = _paramNames[i];
			param["type"] = _paramTypes[i];
			params.append(param);
		}
		return params;
	};

	for (auto it: _contractDef.interfaceFunctions())
	{
		auto externalFunctionType = it.second->interfaceFunctionType();
		Json::Value method;
		method["type"] = "function";
		method["name"] = it.second->declaration().name();
		method["constant"] = it.second->isConstant();
		method["inputs"] = populateParameters(
			externalFunctionType->parameterNames(),
			externalFunctionType->parameterTypeNames(_contractDef.isLibrary())
		);
		method["outputs"] = populateParameters(
			externalFunctionType->returnParameterNames(),
			externalFunctionType->returnParameterTypeNames(_contractDef.isLibrary())
		);
		abi.append(method);
	}
	if (_contractDef.constructor())
	{
		Json::Value method;
		method["type"] = "constructor";
		auto externalFunction = FunctionType(*_contractDef.constructor()).interfaceFunctionType();
		solAssert(!!externalFunction, "");
		method["inputs"] = populateParameters(
			externalFunction->parameterNames(),
			externalFunction->parameterTypeNames(_contractDef.isLibrary())
		);
		abi.append(method);
	}

	for (auto const& it: _contractDef.interfaceEvents())
	{
		Json::Value event;
		event["type"] = "event";
		event["name"] = it->name();
		event["anonymous"] = it->isAnonymous();
		Json::Value params(Json::arrayValue);
		for (auto const& p: it->parameters())
		{
			Json::Value input;
			input["name"] = p->name();
			input["type"] = p->annotation().type->canonicalName(false);
			input["indexed"] = p->isIndexed();
			params.append(input);
		}
		event["inputs"] = params;
		abi.append(event);
	}
	return Json::FastWriter().write(abi);
}

string InterfaceHandler::ABISolidityInterface(ContractDefinition const& _contractDef)
{
	string ret = (_contractDef.isLibrary() ? "library " : "contract ") + _contractDef.name() + "{";

	auto populateParameters = [](vector<string> const& _paramNames, vector<string> const& _paramTypes)
	{
		string ret = "(";
		for (size_t i = 0; i < _paramNames.size(); ++i)
			ret += _paramTypes[i] + " " + _paramNames[i] + ",";
		if (ret.size() != 1)
			ret.pop_back();
		return ret + ")";
	};
	// If this is a library, include all its enum and struct types. Should be more intelligent
	// in the future and check what is actually used (it might even use types from other libraries
	// or contracts or in the global scope).
	if (_contractDef.isLibrary())
	{
		for (auto const& stru: _contractDef.definedStructs())
		{
			ret += "struct " + stru->name() + "{";
			for (ASTPointer<VariableDeclaration> const& _member: stru->members())
				ret += _member->type(nullptr)->canonicalName(false) + " " + _member->name() + ";";
			ret += "}";
		}
		for (auto const& enu: _contractDef.definedEnums())
		{
			ret += "enum " + enu->name() + "{";
			for (ASTPointer<EnumValue> const& val: enu->members())
				ret += val->name() + ",";
			if (ret.back() == ',')
				ret.pop_back();
			ret += "}";
		}
	}
	if (_contractDef.constructor())
	{
		auto externalFunction = FunctionType(*_contractDef.constructor()).interfaceFunctionType();
		solAssert(!!externalFunction, "");
		ret +=
			"function " +
			_contractDef.name() +
			populateParameters(
				externalFunction->parameterNames(),
				externalFunction->parameterTypeNames(_contractDef.isLibrary())
			) +
			";";
	}
	for (auto const& it: _contractDef.interfaceFunctions())
	{
		ret += "function " + it.second->declaration().name() +
			populateParameters(
				it.second->parameterNames(),
				it.second->parameterTypeNames(_contractDef.isLibrary())
			) + (it.second->isConstant() ? "constant " : "");
		if (it.second->returnParameterTypes().size())
			ret += "returns" + populateParameters(
				it.second->returnParameterNames(),
				it.second->returnParameterTypeNames(_contractDef.isLibrary())
			);
		else if (ret.back() == ' ')
			ret.pop_back();
		ret += ";";
	}

	return ret + "}";
}

string InterfaceHandler::userDocumentation(ContractDefinition const& _contractDef)
{
	Json::Value doc;
	Json::Value methods(Json::objectValue);

	for (auto const& it: _contractDef.interfaceFunctions())
	{
		Json::Value user;
		auto strPtr = it.second->documentation();
		if (strPtr)
		{
			resetUser();
			parseDocString(*strPtr, CommentOwner::Function);
			if (!m_notice.empty())
			{// since @notice is the only user tag if missing function should not appear
				user["notice"] = Json::Value(m_notice);
				methods[it.second->externalSignature()] = user;
			}
		}
	}
	doc["methods"] = methods;

	return Json::StyledWriter().write(doc);
}

string InterfaceHandler::devDocumentation(ContractDefinition const& _contractDef)
{
	// LTODO: Somewhere in this function warnings for mismatch of param names
	// should be thrown
	Json::Value doc;
	Json::Value methods(Json::objectValue);

	auto contractDoc = _contractDef.documentation();
	if (contractDoc)
	{
		m_contractAuthor.clear();
		m_title.clear();
		parseDocString(*contractDoc, CommentOwner::Contract);

		if (!m_contractAuthor.empty())
			doc["author"] = m_contractAuthor;

		if (!m_title.empty())
			doc["title"] = m_title;
	}

	for (auto const& it: _contractDef.interfaceFunctions())
	{
		Json::Value method;
		auto strPtr = it.second->documentation();
		if (strPtr)
		{
			resetDev();
			parseDocString(*strPtr, CommentOwner::Function);

			if (!m_dev.empty())
				method["details"] = Json::Value(m_dev);

			if (!m_author.empty())
				method["author"] = m_author;

			Json::Value params(Json::objectValue);
			vector<string> paramNames = it.second->parameterNames();
			for (auto const& pair: m_params)
			{
				if (find(paramNames.begin(), paramNames.end(), pair.first) == paramNames.end())
					// LTODO: mismatching parameter name, throw some form of warning and not just an exception
					BOOST_THROW_EXCEPTION(
						Error(Error::Type::DocstringParsingError) <<
						errinfo_comment("documented parameter \"" + pair.first + "\" not found in the parameter list of the function.")
					);					
				params[pair.first] = pair.second;
			}

			if (!m_params.empty())
				method["params"] = params;

			if (!m_return.empty())
				method["return"] = m_return;

			if (!method.empty()) // add the function, only if we have any documentation to add
				methods[it.second->externalSignature()] = method;
		}
	}
	doc["methods"] = methods;

	return Json::StyledWriter().write(doc);
}

/* -- private -- */
void InterfaceHandler::resetUser()
{
	m_notice.clear();
}

void InterfaceHandler::resetDev()
{
	m_dev.clear();
	m_author.clear();
	m_return.clear();
	m_params.clear();
}

static inline string::const_iterator skipLineOrEOS(
	string::const_iterator _nlPos,
	string::const_iterator _end
)
{
	return (_nlPos == _end) ? _end : ++_nlPos;
}

string::const_iterator InterfaceHandler::parseDocTagLine(
	string::const_iterator _pos,
	string::const_iterator _end,
	string& _tagString,
	DocTagType _tagType,
	bool _appending
)
{
	auto nlPos = find(_pos, _end, '\n');
	if (_appending && _pos < _end && *_pos != ' ')
		_tagString += " ";
	copy(_pos, nlPos, back_inserter(_tagString));
	m_lastTag = _tagType;
	return skipLineOrEOS(nlPos, _end);
}

string::const_iterator InterfaceHandler::parseDocTagParam(
	string::const_iterator _pos,
	string::const_iterator _end
)
{
	// find param name
	auto currPos = find(_pos, _end, ' ');
	if (currPos == _end)
		BOOST_THROW_EXCEPTION(Error(Error::Type::DocstringParsingError) << errinfo_comment("End of param name not found" + string(_pos, _end)));


	auto paramName = string(_pos, currPos);

	currPos += 1;
	auto nlPos = find(currPos, _end, '\n');
	auto paramDesc = string(currPos, nlPos);
	m_params.push_back(make_pair(paramName, paramDesc));

	m_lastTag = DocTagType::Param;
	return skipLineOrEOS(nlPos, _end);
}

string::const_iterator InterfaceHandler::appendDocTagParam(
	string::const_iterator _pos,
	string::const_iterator _end
)
{
	// Should never be called with an empty vector
	solAssert(!m_params.empty(), "Internal: Tried to append to empty parameter");

	auto pair = m_params.back();
	if (_pos < _end && *_pos != ' ')
		pair.second += " ";
	auto nlPos = find(_pos, _end, '\n');
	copy(_pos, nlPos, back_inserter(pair.second));

	m_params.at(m_params.size() - 1) = pair;

	return skipLineOrEOS(nlPos, _end);
}

string::const_iterator InterfaceHandler::parseDocTag(
	string::const_iterator _pos,
	string::const_iterator _end,
	string const& _tag,
	CommentOwner _owner
)
{
	// LTODO: need to check for @(start of a tag) between here and the end of line
	// for all cases. Also somehow automate list of acceptable tags for each
	// language construct since current way does not scale well.
	if (m_lastTag == DocTagType::None || _tag != "")
	{
		if (_tag == "dev")
			return parseDocTagLine(_pos, _end, m_dev, DocTagType::Dev, false);
		else if (_tag == "notice")
			return parseDocTagLine(_pos, _end, m_notice, DocTagType::Notice, false);
		else if (_tag == "return")
			return parseDocTagLine(_pos, _end, m_return, DocTagType::Return, false);
		else if (_tag == "author")
		{
			if (_owner == CommentOwner::Contract)
				return parseDocTagLine(_pos, _end, m_contractAuthor, DocTagType::Author, false);
			else if (_owner == CommentOwner::Function)
				return parseDocTagLine(_pos, _end, m_author, DocTagType::Author, false);
			else
				// LTODO: for now this else makes no sense but later comments will go to more language constructs
				BOOST_THROW_EXCEPTION(Error(Error::Type::DocstringParsingError) << errinfo_comment("@author tag is legal only for contracts"));
		}
		else if (_tag == "title")
		{
			if (_owner == CommentOwner::Contract)
				return parseDocTagLine(_pos, _end, m_title, DocTagType::Title, false);
			else
				// LTODO: Unknown tag, throw some form of warning and not just an exception
				BOOST_THROW_EXCEPTION(Error(Error::Type::DocstringParsingError) << errinfo_comment("@title tag is legal only for contracts"));
		}
		else if (_tag == "param")
			return parseDocTagParam(_pos, _end);
		else
			// LTODO: Unknown tag, throw some form of warning and not just an exception
			BOOST_THROW_EXCEPTION(Error(Error::Type::DocstringParsingError) << errinfo_comment("Unknown tag " + _tag + " encountered"));
	}
	else
		return appendDocTag(_pos, _end, _owner);
}

string::const_iterator InterfaceHandler::appendDocTag(
	string::const_iterator _pos,
	string::const_iterator _end,
	CommentOwner _owner
)
{
	switch (m_lastTag)
	{
	case DocTagType::Dev:
		return parseDocTagLine(_pos, _end, m_dev, DocTagType::Dev, true);
	case DocTagType::Notice:
		return parseDocTagLine(_pos, _end, m_notice, DocTagType::Notice, true);
	case DocTagType::Return:
		return parseDocTagLine(_pos, _end, m_return, DocTagType::Return, true);
	case DocTagType::Author:
		if (_owner == CommentOwner::Contract)
			return parseDocTagLine(_pos, _end, m_contractAuthor, DocTagType::Author, true);
		else if (_owner == CommentOwner::Function)
			return parseDocTagLine(_pos, _end, m_author, DocTagType::Author, true);
		else
			// LTODO: Unknown tag, throw some form of warning and not just an exception
			BOOST_THROW_EXCEPTION(Error(Error::Type::DocstringParsingError) << errinfo_comment("@author tag in illegal comment"));
	case DocTagType::Title:
		if (_owner == CommentOwner::Contract)
			return parseDocTagLine(_pos, _end, m_title, DocTagType::Title, true);
		else
			// LTODO: Unknown tag, throw some form of warning and not just an exception
			BOOST_THROW_EXCEPTION(Error(Error::Type::DocstringParsingError) << errinfo_comment("@title tag in illegal comment"));
	case DocTagType::Param:
		return appendDocTagParam(_pos, _end);
	default:
		BOOST_THROW_EXCEPTION(InternalCompilerError() << errinfo_comment("Internal: Illegal documentation tag type"));
		break;
	}
}

static inline string::const_iterator firstSpaceOrNl(
	string::const_iterator _pos,
	string::const_iterator _end
)
{
	auto spacePos = find(_pos, _end, ' ');
	auto nlPos = find(_pos, _end, '\n');
	return (spacePos < nlPos) ? spacePos : nlPos;
}

void InterfaceHandler::parseDocString(string const& _string, CommentOwner _owner)
{
	auto currPos = _string.begin();
	auto end = _string.end();

	while (currPos != end)
	{
		auto tagPos = find(currPos, end, '@');
		auto nlPos = find(currPos, end, '\n');

		if (tagPos != end && tagPos < nlPos)
		{
			// we found a tag
			auto tagNameEndPos = firstSpaceOrNl(tagPos, end);
			if (tagNameEndPos == end)
				BOOST_THROW_EXCEPTION(
					Error(Error::Type::DocstringParsingError) <<
					errinfo_comment("End of tag " + string(tagPos, tagNameEndPos) + "not found"));

			currPos = parseDocTag(tagNameEndPos + 1, end, string(tagPos + 1, tagNameEndPos), _owner);
		}
		else if (m_lastTag != DocTagType::None) // continuation of the previous tag
			currPos = appendDocTag(currPos, end, _owner);
		else if (currPos != end)
		{
			// if it begins without a tag then consider it as @notice
			if (currPos == _string.begin())
			{
				currPos = parseDocTag(currPos, end, "notice", CommentOwner::Function);
				continue;
			}
			else if (nlPos == end) //end of text
				return;
			// else skip the line if a newline was found and we get here
			currPos = nlPos + 1;
		}
	}
}

} //solidity NS
} // dev NS
