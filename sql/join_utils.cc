/** join_utils.cc                                                  -*- C++ -*-
    Jeremy Barnes, 24 August 2015
    Copyright (c) 2015 Datacratic Inc.  All rights reserved.

    This file is part of MLDB. Copyright 2015 Datacratic. All rights reserved.
*/

#include "join_utils.h"
#include "mldb/types/vector_description.h"
#include "mldb/types/enum_description.h"
#include "mldb/types/structure_description.h"
#include "mldb/sql/sql_expression_operations.h"
#include "mldb/http/http_exception.h"
#include <algorithm>

using namespace std;

namespace Datacratic {
namespace MLDB {

/** Turn a list of ANDed clauses into an actual where clause. */

std::shared_ptr<SqlExpression>
generateWhereExpression(const std::vector<AnnotatedClause> & clause,
                        const Utf8String & tableName)
{
    std::shared_ptr<SqlExpression> result;

    for (auto & c: clause) {
        set<Utf8String> aliases;
        auto tableClause = removeTableName(*c.expr, tableName, aliases);
        if (!result)
            result = tableClause;
        else result = std::make_shared<BooleanOperatorExpression>(result, tableClause, "AND");
    }

    if (!result)
        result.reset(new ConstantExpression(ExpressionValue(true, Date::notADate())));
    
    result->surface = result->print();

    return result;
}

//Check if the idenfifier refers to this dataset and if so, remove the prefix.
bool
extractTableName(Utf8String & variableName, const Utf8String& tableName)
{    
    if (!tableName.empty() && !variableName.empty()) {

        Utf8String aliasWithDot = tableName + ".";

        if (!variableName.removePrefix(aliasWithDot)) {

            //maybe there are quotes around the prefix.             

            auto it1 = variableName.begin();
            if (*it1 == '\"') {
                
                aliasWithDot = "\"" + tableName + "\"" + ".";

                if (variableName.removePrefix(aliasWithDot))
                    return true;             
            }  
        }
        else  {
            return true;
        }
    }   

    return false;
}

bool
extractTableName(Utf8String & variableName, const std::set<Utf8String> & tables)
{
    for (const Utf8String& tableName : tables)
    {
        if (extractTableName(variableName, tableName))
            return true;
    }
    return false;
}


// Replace all variable names like "table.x" with "x" to be run
// in the context of a table
std::shared_ptr<SqlExpression>
removeTableName(const SqlExpression & expr, const Utf8String & tableName, const std::set<Utf8String>& childAliases)
{
    //prefixes to look for in variable names
    //variable names use only the table name not the child aliases
    //because the child columns will have been flattened
    // i.e. if child table x has a column y, the dataset will have a x.y column
    Utf8String prefixToFind = tableName + ".";
    size_t prefixLength = prefixToFind.length();
    Utf8String prefixToFind2 = "\"" + tableName + "\"" + ".";
    size_t prefixLength2 = prefixToFind2.length();

    std::vector<std::tuple<Utf8String, Utf8String, size_t> >  variablePrefixes = 
            {(std::make_tuple(tableName, std::move(prefixToFind), prefixLength)), 
             (std::make_tuple(tableName, std::move(prefixToFind2), prefixLength2)) };

    //build list of prefixes to look for in function names
    //with a tuple of table-name, prefix, prefix length
    std::vector<std::tuple<Utf8String, Utf8String, size_t> > functionprefixes;
    for (const Utf8String& alias : childAliases)
    {
        Utf8String prefixToFind = alias + ".";
        size_t length = prefixToFind.length();
        functionprefixes.push_back(std::make_tuple(alias, std::move(prefixToFind), length));

        Utf8String prefixToFind2 = "\"" + alias + "\"" + ".";
        length = prefixToFind.length();
        functionprefixes.push_back(std::make_tuple(alias, std::move(prefixToFind2), length));
    }

    // If an expression refers to a variable, then remove the table name
    // from it.  Otherwise return the same expression.
    std::function<std::shared_ptr<SqlExpression> (std::shared_ptr<SqlExpression> a)>
        doArg;

    // Function used by transform to allow a traversal of the expression
    // tree, replacing the reading of table.var with var
    auto onChild = [&] (std::vector<std::shared_ptr<SqlExpression> > args)
        {
            for (auto & a: args) {
                a = doArg(a);
            }
            
            return std::move(args);
        };

    doArg = [&] (std::shared_ptr<SqlExpression> a)
        -> std::shared_ptr<SqlExpression>
        {
            auto var = std::dynamic_pointer_cast<ReadVariableExpression>(a);
            if (var) {

                for (auto& prefixt : variablePrefixes)
                {
                     if (var->variableName.startsWith(std::get<1>(prefixt))) {
                        Utf8String name(var->variableName);
                        name.replace(0, std::get<2>(prefixt), Utf8String());
                        ExcAssert(!name.empty());
                        ExcAssert(!name.startsWith("."));
                        return std::make_shared<ReadVariableExpression>(std::get<0>(prefixt), name);
                    }
                }               
            }
            auto func = std::dynamic_pointer_cast<FunctionCallWrapper>(a);
            if (func) {
                for (auto& prefixt : functionprefixes)
                {
                    if (func->functionName.startsWith(std::get<1>(prefixt))) {
                        Utf8String name(func->functionName);
                        name.replace(0, std::get<2>(prefixt), Utf8String());
                        ExcAssert(!name.empty());
                        ExcAssert(!name.startsWith("."));

                        vector<std::shared_ptr<SqlExpression> > newArgs;
                        for (auto & a: func->args) {
                            newArgs.emplace_back(std::move(doArg(a)));
                        }
                        return std::make_shared<FunctionCallWrapper>(std::get<0>(prefixt), name, std::move(newArgs), func->extract);
                    }
                }
            }

            return a->transform(onChild);
        };

    // Walk the tree.  The doArg() on the outside is to deal with the case
    // where there is just a read variable expression, in which case
    // onChild will do nothing.
    auto result = doArg(expr.transform(onChild));

    result->surface = result->print();

    return result;
}

/*****************************************************************************/
/* ANNOTATED CLAUSE                                                          */
/*****************************************************************************/

AnnotatedClause::
AnnotatedClause(std::shared_ptr<SqlExpression> c,
                const std::set<Utf8String> & leftTables,
                const std::set<Utf8String> & rightTables,
                bool debug)
    : expr(c), pivot(UNKNOWN)
{
    auto vars = c->variableNames();
    auto funcs = c->functionNames();
    
    // Which table does it refer to?
    for (auto & var: vars) {
        Utf8String v = var.first.name;

        if (extractTableName(v,leftTables)) {
            ExcAssert(!v.empty());
            ExcAssert(!v.startsWith("."));
            leftVars.emplace_back(std::move(v));
        }
        else if (extractTableName(v,rightTables)) {
            ExcAssert(!v.empty());
            ExcAssert(!v.startsWith("."));
            rightVars.emplace_back(std::move(v));
        }
        else {
            externalVars.emplace_back(v);
        }
    }
    
    for (auto & func: funcs) {
        Utf8String v = func.first.name;
        auto dotIt = v.find('.');
        if (dotIt == v.end()) {
            externalFuncs.emplace_back(v);
            continue;
        }

        Utf8String tableName(v.begin(), dotIt);
        if (leftTables.count(tableName)) {
            v.replace(0, tableName.length() + 1,
                      Utf8String(""));
            ExcAssert(!v.empty());
            ExcAssert(!v.startsWith("."));
            leftFuncs.emplace_back(std::move(v));
        }
        else if (rightTables.count(tableName)) {
            v.replace(0, tableName.length() + 1,
                      Utf8String(""));
            ExcAssert(!v.empty());
            ExcAssert(!v.startsWith("."));
            rightFuncs.emplace_back(std::move(v));
        }
        else externalFuncs.emplace_back(v);
        
    }

    if (leftVars.empty() && rightVars.empty()
        && leftFuncs.empty() && rightFuncs.empty())
        role = CONSTANT;
    else if (leftVars.empty() && leftFuncs.empty())
        role = RIGHT;
    else if (rightVars.empty() && rightFuncs.empty())
        role = LEFT;
    else role = CROSS;
}

DEFINE_ENUM_DESCRIPTION_NAMED(AnnotatedClauseRoleDescription, AnnotatedClause::Role);

AnnotatedClauseRoleDescription::
AnnotatedClauseRoleDescription()
{
    addValue("CONSTANT", AnnotatedClause::CONSTANT, "");
    addValue("LEFT", AnnotatedClause::LEFT, "");
    addValue("RIGHT", AnnotatedClause::RIGHT, "");
    addValue("CROSS", AnnotatedClause::CROSS, "");
}

DEFINE_ENUM_DESCRIPTION_NAMED(AnnotatedClausePivotDescription,
                              AnnotatedClause::Pivot);

AnnotatedClausePivotDescription::
AnnotatedClausePivotDescription()
{
    addValue("UNKNOWN", AnnotatedClause::UNKNOWN, "Pivot status not analysed");
    addValue("NOT_PIVOT", AnnotatedClause::NOT_PIVOT,
             "Not a pivot term ");
    addValue("CHOSEN_PIVOT", AnnotatedClause::CHOSEN_PIVOT,
             "Chosen as the main pivot term");
    addValue("POSSIBLE_PIVOT", AnnotatedClause::POSSIBLE_PIVOT,
             "Possible pivot but not chosen");
}

DEFINE_STRUCTURE_DESCRIPTION(AnnotatedClause);

AnnotatedClauseDescription::
AnnotatedClauseDescription()
{
    addField("expr", &AnnotatedClause::expr, "Expression in clause");
    addField("leftVars", &AnnotatedClause::leftVars, "Variables referring to left table");
    addField("rightVars", &AnnotatedClause::rightVars, "Variables referring to right table");
    addField("externalVars", &AnnotatedClause::externalVars, "Variables referring to neither right or left table");
    addField("leftFuncs", &AnnotatedClause::leftFuncs, "Functions referring to left table");
    addField("rightFuncs", &AnnotatedClause::rightFuncs, "Functions referring to right table");
    addField("externalFuncs", &AnnotatedClause::externalFuncs, "Functions referring to neither right or left table");
    addField("role", &AnnotatedClause::role, "Role of clause in join");
    addField("pivot", &AnnotatedClause::pivot, "Pivot status of clause in join",
             AnnotatedClause::UNKNOWN);

}


/*****************************************************************************/
/* ANNOTATED JOIN CONDITION                                                  */
/*****************************************************************************/

AnnotatedJoinCondition::
AnnotatedJoinCondition()
    : debug(false), style(UNKNOWN)
{
}

AnnotatedJoinCondition::
AnnotatedJoinCondition(std::shared_ptr<TableExpression> leftTable,
                       std::shared_ptr<TableExpression> rightTable,
                       std::shared_ptr<SqlExpression> on,
                       std::shared_ptr<SqlExpression> where,
                       bool debug)
    : debug(debug), on(on), where(where),
      style(UNKNOWN), left(leftTable), right(rightTable)
{
    std::set<Utf8String> leftTables = leftTable->getTableNames();
    std::set<Utf8String> rightTables = rightTable->getTableNames();
        
    std::vector<Utf8String> commonTables;
    std::set_intersection(leftTables.begin(), leftTables.end(),
                          rightTables.begin(), rightTables.end(),
                          std::back_inserter(commonTables));

    if (!commonTables.empty()) {
        throw HttpReturnException(400, "Join has ambiguous table name(s) with same name occurring on both left and right side",
                                  "ambiguousTableNames", commonTables,
                                  "left", left,
                                  "right", right,
                                  "on", on);
    }
    

    // No join clause = cross join
    if (on)
        analyze(on);
    if (where)
        analyze(where);

    if (debug) {
        cerr << "got " << andClauses.size() << " and clauses" << endl;
        cerr << jsonEncode(andClauses) << endl;
    }

    // For each one, figure out which tables are referred to
    for (auto & c: andClauses) {
        if (debug)
            cerr << "clause " << c->print() << endl;
        AnnotatedClause clauseOut(c, leftTables, rightTables);

        if (!clauseOut.externalVars.empty()) {
            throw HttpReturnException(400, "Join condition refers to unknown variables",
                                      "unknownVariables", clauseOut.externalVars,
                                      "clause", c,
                                      "left", left,
                                      "right",right,
                                      "on", on);
        }
            
        switch (clauseOut.role) {
        case AnnotatedClause::CONSTANT:
            constantConditions.emplace_back(std::move(clauseOut));
            break;
        case AnnotatedClause::LEFT:
            left.whereClauses.emplace_back(std::move(clauseOut));
            break;
        case AnnotatedClause::RIGHT:
            right.whereClauses.emplace_back(std::move(clauseOut));
            break;
        case AnnotatedClause::CROSS:
            crossConditions.emplace_back(std::move(clauseOut));
            break;
        }
    }

    left.where = generateWhereExpression(left.whereClauses,
                                         leftTable->getAs());
    right.where = generateWhereExpression(right.whereClauses,
                                          rightTable->getAs());

    //WHEN is not supported in JOIN
    left.when = WhenExpression::parse("true");
    right.when = WhenExpression::parse("true");

    constantWhere = generateWhereExpression(constantConditions, "");

    std::vector<AnnotatedClause> nonPivotWhere;

    if (crossConditions.size() == 0) {
        // OK, we have a cross join.  Lotsa fun.  Generate all rows from each,
        // with "true" as the join clause
        style = CROSS_JOIN;
        left.equalExpression
            = right.equalExpression
            = SqlExpression::parse("true");
    }
    else {
        for (auto & c: crossConditions) {
            // Look for an equality condition in the cross condition
            auto cmpExpr = std::dynamic_pointer_cast<ComparisonExpression>(c.expr);

            if (cmpExpr && cmpExpr->op == "=") {
                
                // We have (expr) == (expr).  We need to check that the left side
                // only refers to things on the left, and the right side to
                // things on the right.
            
                AnnotatedClause leftAnnotated(cmpExpr->lhs, leftTables, rightTables);
                AnnotatedClause rightAnnotated(cmpExpr->rhs, leftTables, rightTables);
                if (debug)
                    cerr << "leftAnnotated = " << jsonEncode(leftAnnotated)
                         << " rightAnnotated = " << jsonEncode(rightAnnotated)
                         << endl;
            
            
                if (leftAnnotated.role == AnnotatedClause::RIGHT)
                    std::swap(leftAnnotated, rightAnnotated);
            
                if (leftAnnotated.role != AnnotatedClause::LEFT
                    || rightAnnotated.role != AnnotatedClause::RIGHT)
                    continue;

                c.pivot = AnnotatedClause::POSSIBLE_PIVOT;

                if (style == EQUIJOIN) {
                    nonPivotWhere.push_back(c);
                    continue;
                }
                
                style = EQUIJOIN;
                c.pivot = AnnotatedClause::CHOSEN_PIVOT;
                left.equalExpression = leftAnnotated.expr;
                right.equalExpression = rightAnnotated.expr;
            }
            else {
                nonPivotWhere.push_back(c);
            }
        }

        if (style != EQUIJOIN) {
            throw HttpReturnException
                (400, "Could not find join clause pivot: "
                 "Join condition must have one clause of form left.var1 = right.var2",
                 "conditions", *this);
        }
    }

    crossWhere = generateWhereExpression(nonPivotWhere, "");

    auto doSide = [&] (Side & side)
        {
            // Remove the "table." from "table.var" as we are running the
            // expression locally to the table, not in the context of the
            // join.

            auto localExpr = removeTableName(*side.equalExpression, side.table->getAs(), side.table->getTableNames());
            side.selectExpression = localExpr;

            // Construct the select expression.  It's simply the value of
            // the join expression.
            side.select.clauses.push_back
            (std::make_shared<ComputedVariable>(Utf8String("val"),
                                                localExpr));
            side.select.surface = side.select.print();
            
            // Construct the order by expression.  Again, it's simply the
            // value of the join expression.
            side.orderBy.clauses.emplace_back(localExpr, ASC);
        };

    doSide(left);
    doSide(right);
}

void
AnnotatedJoinCondition::
analyze(const std::shared_ptr<SqlExpression> & expr)
{
    if (debug)
        cerr << "analyzing " << expr->surface << " " << expr->print() << endl;

    auto boolExpr = std::dynamic_pointer_cast<BooleanOperatorExpression>(expr);
                
    if (boolExpr) {
        if (debug)
            cerr << "operation " << boolExpr->op << endl;
        if (boolExpr->op == "AND") {
            analyze(boolExpr->lhs);
            analyze(boolExpr->rhs);
            return;
        }
    }
    andClauses.push_back(expr);
}

DEFINE_STRUCTURE_DESCRIPTION_NAMED(AnnotatedJoinConditionSideDescription,
                                   AnnotatedJoinCondition::Side);

AnnotatedJoinConditionSideDescription::
AnnotatedJoinConditionSideDescription()
{
    addField("table", &AnnotatedJoinCondition::Side::table,
             "Table underlying this side");
    addField("whereClauses", &AnnotatedJoinCondition::Side::whereClauses,
             "WHERE clauses for the  side");
    addField("equalExpression", &AnnotatedJoinCondition::Side::equalExpression,
             "Equality expression for the  side");
    addField("selectExpression", &AnnotatedJoinCondition::Side::selectExpression,
             "Select expression for the equi-join part of the side");
    addField("where", &AnnotatedJoinCondition::Side::where,
             "WHERE clauses for the  side");
    addField("select", &AnnotatedJoinCondition::Side::select,
             "SELECT clauses for the  side");
    addField("orderBy", &AnnotatedJoinCondition::Side::orderBy,
             "ORDER BY clauses for the  side");

}

DEFINE_STRUCTURE_DESCRIPTION(AnnotatedJoinCondition);

AnnotatedJoinConditionDescription::
AnnotatedJoinConditionDescription()
{
    addField("on", &AnnotatedJoinCondition::on,
             "Original ON clause");
    addField("where", &AnnotatedJoinCondition::where,
             "Original WHERE clause");
    addField("andClauses", &AnnotatedJoinCondition::andClauses,
             "Clauses separated by AND");
    
    addField("style", &AnnotatedJoinCondition::style,
             "Style of join");

    addField("left", &AnnotatedJoinCondition::left,
             "Left table of the join");
    addField("right", &AnnotatedJoinCondition::right,
             "Right table of the join");

    addField("crossConditions", &AnnotatedJoinCondition::crossConditions,
             "Conditions involving both sides");
    addField("constantConditions", &AnnotatedJoinCondition::constantConditions,
             "Conditions involving neither side");

    addField("crossWhere", &AnnotatedJoinCondition::crossWhere,
             "Filtered where expression for cross join");
    addField("constantWhere", &AnnotatedJoinCondition::constantWhere,
             "Where expression independent of either table");
}

DEFINE_ENUM_DESCRIPTION_NAMED(AnnotatedJoinConditionStyleDescription,
                              AnnotatedJoinCondition::Style);

AnnotatedJoinConditionStyleDescription::
AnnotatedJoinConditionStyleDescription()
{
    addValue("EMPTY", AnnotatedJoinCondition::EMPTY, "Returns nothing");
    addValue("CROSS_JOIN", AnnotatedJoinCondition::CROSS_JOIN, "Cross-join");
    addValue("EQUIJOIN", AnnotatedJoinCondition::EQUIJOIN, "Join on equality");
    addValue("UNKNOWN", AnnotatedJoinCondition::UNKNOWN, "Unknown join type");
}

} // namespace MLDB
} // namespace Datacratic
