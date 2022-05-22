#include <unistd.h>
#include <iostream>
#include <fstream>
#include <ctime>
#include <string>
#include <memory>

#include <clang/Tooling/CommonOptionsParser.h>

#include "oclint/Analyzer.h"
#include "oclint/CompilerInstance.h"
#include "oclint/Constants.h"
#include "oclint/Driver.h"
#include "oclint/ExitCode.h"
#include "oclint/GenericException.h"
#include "oclint/Options.h"
#include "oclint/RawResults.h"
#include "oclint/Reporter.h"
#include "oclint/ResultCollector.h"
#include "oclint/RuleBase.h"
#include "oclint/RuleSet.h"
#include "oclint/RulesetFilter.h"
#include "oclint/RulesetBasedAnalyzer.h"
#include "oclint/UniqueResults.h"
#include "oclint/ViolationSet.h"
#include "oclint/Violation.h"

#include "reporters.h"
#include "rules.h"

using namespace std;
using namespace llvm;
using namespace clang;
using namespace clang::tooling;

void consumeArgRulesPath()
{
    for (const auto& rulePath : oclint::option::rulesPath())
    {
        dynamicLoadRules(rulePath);
    }
}

bool numberOfViolationsExceedThreshold(oclint::Results *results)
{
    return results->numberOfViolationsWithPriority(1) > oclint::option::maxP1() ||
        results->numberOfViolationsWithPriority(2) > oclint::option::maxP2() ||
        results->numberOfViolationsWithPriority(3) > oclint::option::maxP3();
}

ostream* outStream(oclint::Reporter* reporter)
{
    if (!oclint::option::hasOutputPath())
    {
        return &cout;
    }

    // the original outputPath should be like that: "dir1/dir2/name.*"
    // Or due to the old config file, like that: "dir1/dir2/name.html", "dir1/dir2/name.xml"
    // We should modify the outputPath so that the name extension matches reporter->name()
    string output = oclint::option::outputPath();
    output = llvm::sys::path::parent_path(output).str() + "/" + llvm::sys::path::stem(output).str() + "." + reporter->name();

    auto out = new ofstream(output.c_str());
    if (!out->is_open())
    {
        throw oclint::GenericException("cannot open report output file " + output);
    }
    return out;
}

void disposeOutStream(ostream* out)
{
    if (out && oclint::option::hasOutputPath())
    {
        ofstream *fout = (ofstream *)out;
        fout->close();
    }
}

void listRules()
{
    cout << "Enabled rules:" << endl;
    for (const std::string &ruleName : oclint::option::rulesetFilter().filteredRuleNames())
    {
        cout << "- " << ruleName << endl;
    }
    cout << endl;
}

void printErrorLine(const char *errorMessage)
{
    cerr << endl << "oclint: error: " << errorMessage << endl;
}

void printViolationsExceedThresholdError(const oclint::Results *results)
{
    printErrorLine("violations exceed threshold");
    cerr << "P1=" << results->numberOfViolationsWithPriority(1)
        << "[" << oclint::option::maxP1() << "] ";
    cerr << "P2=" << results->numberOfViolationsWithPriority(2)
        << "[" << oclint::option::maxP2() << "] ";
    cerr << "P3=" << results->numberOfViolationsWithPriority(3)
        << "[" << oclint::option::maxP3() << "] " <<endl;
}

std::unique_ptr<oclint::Results> getResults()
{
    std::unique_ptr<oclint::Results> results;
    if (oclint::option::allowDuplicatedViolations())
    {
        results.reset(new oclint::RawResults(*oclint::ResultCollector::getInstance()));
    }
    else
    {
        results.reset(new oclint::UniqueResults(*oclint::ResultCollector::getInstance()));
    }
    return results;
}

int prepare()
{
    try
    {
        consumeArgRulesPath();
    }
    catch (const exception& e)
    {
        printErrorLine(e.what());
        return RULE_NOT_FOUND;
    }
    if (oclint::RuleSet::numberOfRules() <= 0)
    {
        printErrorLine("no rule loaded");
        return RULE_NOT_FOUND;
    }
    try
    {
        loadReporter();
    }
    catch (const exception& e)
    {
        printErrorLine(e.what());
        return REPORTER_NOT_FOUND;
    }

    return SUCCESS;
}

static void oclintVersionPrinter(raw_ostream &outs)
{
    outs << "OCLint (" << oclint::Constants::homepage() << "):\n";
    outs << "  OCLint version " << oclint::Constants::version() << ".\n";
    outs << "  Built " << __DATE__ << " (" << __TIME__ << ").\n";
}

extern llvm::cl::OptionCategory OCLintOptionCategory;

int handleExit(oclint::Results *results)
{
    if (results->hasErrors())
    {
        return COMPILATION_ERRORS;
    }

    if (numberOfViolationsExceedThreshold(results))
    {
        printViolationsExceedThresholdError(results);
        return VIOLATIONS_EXCEED_THRESHOLD;
    }

  return SUCCESS;
}

int main(int argc, const char **argv)
{
    llvm::cl::SetVersionPrinter(oclintVersionPrinter);
    auto expectedParser = CommonOptionsParser::create(argc, argv, OCLintOptionCategory);
    if (!expectedParser)
    {
        llvm::errs() << expectedParser.takeError();
        return COMMON_OPTIONS_PARSER_ERRORS;
    }
    CommonOptionsParser &optionsParser = expectedParser.get();
    oclint::option::process(argv[0]);

    int prepareStatus = prepare();
    if (prepareStatus)
    {
        return prepareStatus;
    }

    if (oclint::option::showEnabledRules())
    {
        listRules();
    }

    oclint::RulesetBasedAnalyzer analyzer(oclint::option::rulesetFilter().filteredRules());
    oclint::Driver driver;
    try
    {
        driver.run(optionsParser.getCompilations(), optionsParser.getSourcePathList(), analyzer);
    }
    catch (const exception& e)
    {
        printErrorLine(e.what());
        return ERROR_WHILE_PROCESSING;
    }

    std::unique_ptr<oclint::Results> results(std::move(getResults()));

    try
    {
        for (auto& reporter : reporters()) {
            ostream *out = outStream(reporter);
            reporter->report(results.get(), *out);
            disposeOutStream(out);
        }
    }
    catch (const exception& e)
    {
        printErrorLine(e.what());
        return ERROR_WHILE_REPORTING;
    }

    return handleExit(results.get());
}
