// This file is part of MLDB. Copyright 2015 Datacratic. All rights reserved.

/* separation_stats.cc
   Jeremy Barnes, 14 June 2011
   Copyright (c) 2011 Datacratic.  All rights reserved.

*/

#include "separation_stats.h"
#include "mldb/arch/exception.h"
#include "mldb/base/exc_assert.h"
#include <boost/utility.hpp>
#include "mldb/vfs/filter_streams.h"


using namespace std;
using namespace ML;


namespace Datacratic {

/*****************************************************************************/
/* BINARY STATS                                                              */
/*****************************************************************************/

double
BinaryStats::
rocAreaSince(const BinaryStats & other) const
{
    double tp1 = other.truePositiveRate(), fp1 = other.falsePositiveRate();
    double tp2 = truePositiveRate(), fp2 = falsePositiveRate();

    double result = (fp2 - fp1) * (tp1 + tp2) * 0.5;

    //cerr << "tp1 = " << tp1 << " tp2 = " << tp2 << " fp1 = " << fp1
    //     << " fp2 = " << fp2 << " area = " << result << endl;

    return result;
}

void
BinaryStats::
add(const BinaryStats & other, double weight)
{
    counts[0][0] += weight * other.counts[0][0];
    counts[0][1] += weight * other.counts[0][1];
    counts[1][0] += weight * other.counts[1][0];
    counts[1][1] += weight * other.counts[1][1];
    threshold += weight * other.threshold;
}

Json::Value
BinaryStats::
toJson() const
{
    Json::Value result;
    result["population"]["included"] = includedPopulation();
    result["population"]["excluded"] = excludedPopulation();
    result["pr"]["precision"] = precision();
    result["pr"]["recall"] = recall();
    result["pr"]["f"] = f();
    result["mcc"] = mcc();
    result["gain"] = gain();
    result["counts"]["truePositives"] = truePositives();
    result["counts"]["falsePositives"] = falsePositives();
    result["counts"]["trueNegatives"] = trueNegatives();
    result["counts"]["falseNegatives"] = falseNegatives();
    result["threshold"] = threshold;
    return result;
}


/*****************************************************************************/
/* SCORED STATS                                                              */
/*****************************************************************************/

ScoredStats::
ScoredStats()
    : auc(1.0), isSorted(true)
{
}

BinaryStats
ScoredStats::
atThreshold(float threshold) const
{
    struct FindThreshold {
        bool operator () (const BinaryStats & e1, const BinaryStats & e2) const
        {
            return e1.threshold > e2.threshold;
        }

        bool operator () (const BinaryStats & e1, float e2) const
        {
            return e1.threshold > e2;
        }

        bool operator () (float e1, const BinaryStats & e2) const
        {
            return e1 > e2.threshold;
        }
    };

    if (!std::is_sorted(stats.begin(), stats.end(), FindThreshold()))
        throw Exception("stats not sorted on input");

    if (stats.empty())
        throw Exception("stats is empty");

    // Lower bound means strictly above
    auto lower
        = std::lower_bound(stats.begin(), stats.end(), threshold,
                           FindThreshold());
    
    if (lower == stats.end())
        return stats.back();

    return *lower;
}

BinaryStats
ScoredStats::
atPercentile(float percentile) const
{
    struct FindPercentile {
        bool operator () (const BinaryStats & e1, const BinaryStats & e2) const
        {
            return e1.proportionOfPopulation() < e2.proportionOfPopulation();
        }

        bool operator () (const BinaryStats & e1, float e2) const
        {
            return e1.proportionOfPopulation() < e2;
        }

        bool operator () (float e1, const BinaryStats & e2) const
        {
            return e1 < e2.proportionOfPopulation();
        }
    };

    for (unsigned i = 1;  i < stats.size();  ++i) {
        if (stats[i -1].proportionOfPopulation() > stats[i].proportionOfPopulation()) {
            cerr << "i = " << i << endl;
            cerr << "prev = " << stats[i - 1].toJson() << endl;
            cerr << "ours = " << stats[i].toJson() << endl;
            throw ML::Exception("really not sorted on input");
        }
    }

    if (!std::is_sorted(stats.begin(), stats.end(), FindPercentile()))
        throw Exception("stats not sorted on input");

    if (stats.empty())
        throw Exception("stats is empty");

    // Lower bound means strictly above
    auto it
        = std::lower_bound(stats.begin(), stats.end(), percentile,
                           FindPercentile());

    if (it == stats.begin())
        return *it;

    if (it == stats.end())
        return stats.back();

    BinaryStats upper = *it, lower = *boost::prior(it);

    // Do an interpolation
    double atUpper = upper.proportionOfPopulation(),
           atLower = lower.proportionOfPopulation(),
           range = atUpper - atLower;

    ExcAssertLessEqual(percentile, atUpper);
    ExcAssertGreaterEqual(percentile, atLower);
    ExcAssertGreater(range, 0);

    double weight1 = (atUpper - percentile) / range;
    double weight2 = (percentile - atLower) / range;

    //cerr << "atUpper = " << atUpper << " atLower = " << atLower
    //     << " range = " << range << " weight1 = " << weight1
    //     << " weight2 = " << weight2 << endl;

    BinaryStats result;
    result.add(upper, weight2);
    result.add(lower, weight1);
    
    //cerr << "result prop = " << result.proportionOfPopulation() << endl;

    return result;
}

void
ScoredStats::
sort()
{
    // Go from highest to lowest score
    std::sort(entries.begin(), entries.end());
    isSorted = true;
}

void
ScoredStats::
calculate()
{
    BinaryStats current;
    
    for (unsigned i = 0;  i < entries.size();  ++i)
        current.counts[entries[i].label][false] += entries[i].weight;

    if (!isSorted)
        sort();

    bestF = current;
    bestMcc = current;

    double totalAuc = 0.0;

    stats.clear();

    // take the all point
    stats.push_back(BinaryStats(current, INFINITY));

    for (unsigned i = 0;  i < entries.size();  ++i) {
        const ScoredStats::ScoredEntry & entry = entries[i];

        if (i > 0 && entries[i - 1].score != entry.score) {
            totalAuc += current.rocAreaSince(stats.back());
            stats.push_back
                (BinaryStats(current, entries[i - 1].score, entry.key));

            if (current.f() > bestF.f())
                bestF = stats.back();
            if (current.mcc() > bestMcc.mcc())
                bestMcc = stats.back();
            if (current.specificity() > bestSpecificity.specificity())
                bestSpecificity = stats.back();

            
#if 0
            cerr << "entry " << i << ": score " << entries[i - 1].score
                 << " p " << current.precision() << " r " << current.recall()
                 << " f " << current.f() << " mcc " << current.mcc() << endl;
#endif
        }

        bool label = entry.label;

        // We transfer from a false positive to a true negative, or a
        // true positive to a false negative

        double weight = entry.weight;

        current.counts[label][false] -= weight;
        current.counts[label][true] += weight;

    }
    
    totalAuc += current.rocAreaSince(stats.back());

    if (!entries.empty())
        stats.push_back(BinaryStats(current, entries.back().score));

    bestF = bestF;
    bestMcc = bestMcc;
    auc = totalAuc;
}

void
ScoredStats::
add(const ScoredStats & other)
{
    if (!isSorted)
        throw ML::Exception("attempt to add to non-sorted separation stats");
    if (!other.isSorted)
        throw ML::Exception("attempt to add non-sorted separation stats");

    size_t split = entries.size();
    entries.insert(entries.end(), other.entries.begin(), other.entries.end());
    std::inplace_merge(entries.begin(), entries.begin() + split,
                       entries.end());

    // If we had already calculated, we recalculate
    if (!stats.empty())
        calculate();
}

void
ScoredStats::
dumpRocCurveJs(std::ostream & stream) const
{
    if (stats.empty())
        throw ML::Exception("can't dump ROC curve without calling calculate()");

    stream << "this.data = {" << endl;
    stream << ML::format("  \"aroc\": %8.05f , ", auc) << endl;
    stream << "  \"model\":{";
    for(unsigned i = 0;  i < stats.size();  ++i) {
        const BinaryStats & x = stats[i];
        stream << ML::format("\n  \"%8.05f\": { ", x.threshold);
        stream << ML::format("\n  tpr : %8.05f,", x.recall());
        stream << ML::format("\n  precision : %8.05f,", x.precision());
        stream << ML::format("\n  fscore : %8.05f,", x.f());
        stream << ML::format("\n  fpr : %8.05f,", x.falsePositiveRate());
        stream << ML::format("\n  tp : %8.05f,", x.truePositives());
        stream << ML::format("\n  fp : %8.05f,", x.falsePositives());
        stream << ML::format("\n  fn : %8.05f,", x.falseNegatives());
        stream << ML::format("\n  tn : %8.05f,", x.trueNegatives());
        stream << ML::format("}");
        stream << ",";
    }
    stream << "\n}";
    stream << "};";
}

void
ScoredStats::
saveRocCurveJs(const std::string & filename) const
{
    ML::filter_ostream stream(filename);
    dumpRocCurveJs(stream);
}

Json::Value
ScoredStats::
getRocCurveJson() const
{
    if (stats.empty())
        throw ML::Exception("can't dump ROC curve without calling calculate()");

    Json::Value modelJs;
    for(unsigned i = 0;  i < stats.size();  ++i) {
        const BinaryStats & x = stats[i];
        modelJs[ML::format("%8.05f", x.threshold)] = x.toJson();
    }

    Json::Value js;
    js["aroc"] = auc;
    js["model"] = modelJs;
    js["bestF"] = bestF.toJson();
    js["bestMcc"] = bestMcc.toJson();

    return js;
}

void
ScoredStats::
saveRocCurveJson(const std::string & filename) const
{
    ML::filter_ostream stream(filename);
    stream << getRocCurveJson().toStyledString() << endl;
}

Json::Value
ScoredStats::
toJson() const
{
    Json::Value result;
    result["auc"] = auc;
    result["bestF"] = bestF.toJson();
    result["bestMcc"] = bestMcc.toJson();
    return result;
}

} // namespace Datacratic
