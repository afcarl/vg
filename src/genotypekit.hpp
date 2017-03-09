#ifndef VG_GENOTYPEKIT_H
#define VG_GENOTYPEKIT_H
// genotypekit.hpp: defines pluggable modules for building the genotyper

// The basic idea here is we're going to create a few of these classes, fill in
// their public parameter fields, and then wire them up and set them going to
// emit genotypes in a streaming fashion.

#include <iostream>
#include <algorithm>
#include <functional>
#include <cmath>
#include <regex>
#include <limits>
#include <unordered_set>
#include <unordered_map>
#include <list>
#include "vg.pb.h"
#include "vg.hpp"
#include "translator.hpp"
#include "hash_map.hpp"
#include "utility.hpp"
#include "types.hpp"
#include "bubbles.hpp"
#include "distributions.hpp"
#include "snarls.hpp"
#include "path_index.hpp"

namespace vg {

using namespace std;
    
/**
 * Represents a strategy for finding (nested) sites in a vg graph that can be described
 * by snarls. Polymorphic base class/interface.
 */
class SnarlFinder {
public:
    virtual ~SnarlFinder() = default;
    
    /**
     * Run a function on all root-level NestedSites in parallel. Site trees are
     * passed by value so they have a clear place to live during parallel
     * operations.
     */
    virtual SnarlManager find_snarls() = 0;
};

/**
 * Represents a strategy for finding traversals of (nested) sites. Polymorphic
 * base class/interface.
 */
class TraversalFinder {
public:
    virtual ~TraversalFinder() = default;
    
    virtual vector<SnarlTraversal> find_traversals(const Snarl& site) = 0;
};


/**
 * Represents a strategy for computing consistency between Alignments and
 * SnarlTraversals. Determines whether a read is consistent with a SnarlTraversal
 * or not (but has access to all the SnarlTraversals). Polymorphic base
 * class/interface.
 */
class ConsistencyCalculator {
public:
    virtual ~ConsistencyCalculator() = default;
    
    /**
     * Return true or false for each tarversal of the site, depending on if the
     * read is consistent with it or not.
     */
    virtual vector<bool> calculate_consistency(const Snarl& site,
        const vector<SnarlTraversal>& traversals, const Alignment& read) const = 0;
};

class SimpleConsistencyCalculator : public ConsistencyCalculator{
    public:
    virtual vector<bool> calculate_consistency(const Snarl& site,
        const vector<SnarlTraversal>& traversals, const Alignment& read);
};


/**
 * Represents a strategy for calculating Supports for SnarlTraversals.
 * Polymorphic base class/interface.
 */ 
class TraversalSupportCalculator {
public:
    virtual ~TraversalSupportCalculator() = default;
    
    /**
     * Return Supports for all the SnarlTraversals, given the reads and their
     * consistency flags.
     */
    virtual vector<Support> calculate_supports(const Snarl& site,
        const vector<SnarlTraversal>& traversals, const vector<Alignment*>& reads,
        const vector<vector<bool>>& consistencies) const = 0;
};

// TODO: This needs to be redesigned vis a vis the Genotype object. Genotypes
// need an accompanying Locus object in order to have the Path of the allele
// and also they are not site tree aware.
/**
 * Represents a strategy for calculating genotype likelihood for a (nested)
 * Site. Polymorphic base class/interface.
 */
class GenotypeLikelihoodCalculator {
public:
    virtual ~GenotypeLikelihoodCalculator() = default;
    
    /**
     * Return the log likelihood of the given genotype.
     */
    virtual double calculate_log_likelihood(const Snarl& site,
        const vector<SnarlTraversal>& traversals, const Genotype& genotype,
        const vector<vector<bool>>& consistencies, const vector<Support>& supports,
        const vector<Alignment*>& reads) = 0;
};

/**
 * Represents a strategy for assigning genotype priors. Polymorphic base
 * class/interface.
 */
class GenotypePriorCalculator {
public:
    virtual ~GenotypePriorCalculator() = default;
    
    /**
     * Return the log prior of the given genotype.
     *
     * TODO: ploidy priors on nested sites???
     */
    virtual double calculate_log_prior(const Genotype& genotype) = 0;
};

/**
 * Represents a strategy for converting Locus objects to VCF records.
 * Polymorphic base class/interface.
 */
class VcfRecordConverter {
public:
    virtual ~VcfRecordConverter() = default;
    
    virtual vcflib::Variant convert(const Locus& locus) = 0;
};


/**
 * Represents a filter that passes or rejects VCF records according to some
 * criteria. Polymorphic base class/interface.
 */
class VcfRecordFilter {
public:
    virtual ~VcfRecordFilter() = default;
    
    /**
     * Returns true if we should keep the given VCF record, and false otherwise.
     */
    virtual bool accept_record(const vcflib::Variant& variant) = 0;
};



/////////////////////////////////
// And now the implementations //
/////////////////////////////////

// Represents an assertion that an element in the augmented graph results from
// an event of a certain type.
enum ElementCall {
    CALL_DELETION = 'D',
    CALL_REFERENCE = 'R',
    CALL_UNCALLED = 'U',
    CALL_SUBSTITUTION = 'S',
    CALL_INSERTION = 'I'
};

/// Data structure for representing an augmented graph, with semantic hints
/// about how it was generated and how much support each node and edge has.
struct AugmentedGraph {
    // This holds all the new nodes and edges
    VG graph;
    
    // This holds info about where all the nodes came from
    map<Node*, ElementCall> node_calls;
    // And this similarly holds origin information for the edges
    map<Edge*, ElementCall> edge_calls;
    
    // This holds support info for nodes. Note that we discard the "os" other
    // support field from StrandSupport.
    map<Node*, Support> node_supports;
    // And for edges
    map<Edge*, Support> edge_supports;
    
    // This holds the likelihood for each node.
    // TODO: what exactly does that mean?
    map<Node*, double> node_likelihoods;
    // And for edges
    map<Edge*, double> edge_likelihoods;
    
    // This records how each new node came from the original graph, if it's not
    // just a straight copy. Each Translation is a single mapping for a single
    // whole new node on the forward strand, and the piece of the single old
    // node it came from, on the forward strand.
    vector<Translation> translations;
    
    /**
     * Clear the contents.
     */
    void clear();
};
    
class CactusUltrabubbleFinder : public SnarlFinder {
    
    /// Holds the vg graph we are looking for sites in.
    VG& graph;
    
    /// Use this path name as a rooting hint, if present.
    string hint_path_name;
    
    /// Indicates whether bubbles that consist of a single edge should be filtered
    bool filter_trivial_bubbles;
    
public:
    /**
     * Make a new CactusSiteFinder to find sites in the given graph.
     */
    CactusUltrabubbleFinder(VG& graph,
                            const string& hint_path_name = "",
                            bool filter_trivial_bubbles = false);
    
    /**
     * Find all the sites in parallel with Cactus, make the site tree, and call
     * the given function on all the top-level sites.
     */
    virtual SnarlManager find_snarls();
    
};
    
class ExhaustiveTraversalFinder : public TraversalFinder {
    
    VG& graph;
    SnarlManager& snarl_manager;
    
public:
    ExhaustiveTraversalFinder(VG& graph, SnarlManager& snarl_manager);
    
    virtual ~ExhaustiveTraversalFinder();
    
    /**
     * Exhaustively enumerate all traversals through the site. Only valid for
     * acyclic Snarls.
     */
    virtual vector<SnarlTraversal> find_traversals(const Snarl& site);
    
private:
    void stack_up_valid_walks(NodeTraversal walk_head, vector<NodeTraversal>& stack);
    
};
    
class ReadRestrictedTraversalFinder : TraversalFinder {
    
    VG& graph;
    SnarlManager& snarl_manager;
    const map<string, Alignment*>& reads_by_name;
    
    // How many times must a path recur before we try aligning to it? Also, how
    // many times must a node in the graph be visited before we use it in indel
    // realignment for nearby indels? Note that the primary path counts as a
    // recurrence. TODO: novel inserts can't recur, and novel deletions can't be
    // filtered in this way.
    int min_recurrence;
    
    // How many nodes max should we walk when checking if a path runs through a superbubble/site
    int max_path_search_steps;
    
public:
    ReadRestrictedTraversalFinder(VG& graph, SnarlManager& snarl_manager, const map<string,
                                  Alignment*>& reads_by_name, int min_recurrence = 2,
                                  int max_path_search_steps = 100);
    
    virtual ~ReadRestrictedTraversalFinder();
    
    /**
     * For the given site, emit all traversals with unique sequences that run from
     * start to end, out of the paths in the graph. Uses the map of reads by
     * name to determine if a path is a read or a real named path. Paths through
     * the site supported only by reads are subject to a min recurrence count,
     * while those supported by actual embedded named paths are not.
     */
    virtual vector<SnarlTraversal> find_traversals(const Snarl& site);
    
};

class PathBasedTraversalFinder : public TraversalFinder{
    vg::VG graph;
    public:
    PathBasedTraversalFinder(vg::VG graph);
    virtual ~PathBasedTraversalFinder() = default;
    virtual vector<SnarlTraversal> find_traversals(const Snarl& site);

};

/**
 * This traversal finder finds one or more traversals through leaf sites with no
 * children. It uses a depth-first search. It doesn't work on non-leaf sites,
 * and is not guaranteed to find all traversals. Only works on ultrabubbles.
 */
class TrivialTraversalFinder : public TraversalFinder {

    // Holds the vg graph we are looking for traversals in.
    VG& graph;

public:
    TrivialTraversalFinder(VG& graph);

    virtual ~TrivialTraversalFinder() = default;
    
    /**
     * Find at least one traversal of the site by depth first search, if any
     * exist. Only works on sites with no children.
     */
    virtual vector<SnarlTraversal> find_traversals(const Snarl& site);
};

/**
 * This TraversalFinder is derived from the old vg call code, and emits at least
 * one traversal representing every node, and one traversal representing every
 * edge.
 */
class RepresentativeTraversalFinder : public TraversalFinder {

protected:
    /// The annotated, augmented graph we're finding traversals in
    AugmentedGraph& augmented;
    /// The SnarlManager managiung the snarls we use
    SnarlManager& snarl_manager;
    /// An index of the primary path in the graph, to scaffold the produced traversals.
    PathIndex& index;
    
    /// What DFS depth should we search to?
    size_t max_depth;
    //. How many search intermediates can we allow?
    size_t max_bubble_paths;
    
    /**
     * Given an edge or node in the augmented graph, look out from the edge or
     * node in both directions to find a shortest bubble relative to the path,
     * with a consistent orientation. The bubble may not visit the same node
     * twice.
     *
     * Exactly one of edge and node must be null, and one not null.
     *
     * Takes a max depth for the searches producing the paths on each side.
     * 
     * Return the ordered and oriented nodes in the bubble, with the outer nodes
     * being oriented forward along the named path, and with the first node
     * coming before the last node in the reference.  Also return the minimum
     * support found on any edge or node in the bubble (including the reference
     * node endpoints and their edges which aren't stored in the path)
     */
    pair<Support, vector<NodeTraversal>> find_bubble(Node* node, Edge* edge);
        
    /**
     * Get the minimum support of all nodes and edges in path
     */
    Support min_support_in_path(const list<NodeTraversal>& path);
        
    /**
     * Do a breadth-first search left from the given node traversal, and return
     * lengths and paths starting at the given node and ending on the indexed
     * reference path. Refuses to visit nodes with no support.
     */
    set<pair<size_t, list<NodeTraversal>>> bfs_left(NodeTraversal node, bool stopIfVisited = false);
        
    /**
     * Do a breadth-first search right from the given node traversal, and return
     * lengths and paths starting at the given node and ending on the indexed
     * reference path.
     */
    set<pair<size_t, list<NodeTraversal>>> bfs_right(NodeTraversal node, bool stopIfVisited = false);
        
    /**
     * Get the length of a path through nodes, in base pairs.
     */
    size_t bp_length(const list<NodeTraversal>& path);
    
public:

    RepresentativeTraversalFinder(AugmentedGraph& augmented, SnarlManager& snarl_manager,
        PathIndex& index, size_t max_depth, size_t max_bubble_paths);
    
    /// Should we emit verbose debugging info?
    bool verbose = false;
    
    virtual ~RepresentativeTraversalFinder() = default;
    
    /**
     * Find traversals to cover the nodes and edges of the snarl. Always emits
     * the primary path traversal first, if applicable.
     */
    virtual vector<SnarlTraversal> find_traversals(const Snarl& site);
    
};

/**
 * This genotype prior calculator has a fixed prior for homozygous genotypes and
 * a fixed prior for hets.
 */
class FixedGenotypePriorCalculator : public GenotypePriorCalculator {
public:
    // These parameters are configurable, but have defaults.
    double homozygous_prior_ln = prob_to_logprob(0.999);
    double heterozygous_prior_ln = prob_to_logprob(0.001);


    virtual ~FixedGenotypePriorCalculator() = default;
    virtual double calculate_log_prior(const Genotype& genotype);
};

/**
 * TBD
 *
 */
// class StandardVcfRecordConverter {
// private:
//    const ReferenceIndex& index;
//    vcflib::VariantCallFile& vcf;
//    const string& sample_name;
   
// public:
//    StandardVcfRecordConverter();
//    virtual ~StandardVcfRecordConverter() = default;
   
//    virtual vcflib::Variant convert(const Locus& locus) = 0;
// };
    
// We also supply utility functions for working with genotyping Protobuf objects

/**
 * Get the total read support in a Support.
 */
int total(const Support& support);

/**
 * Get the minimum support of a pair of Supports, by taking the min in each
 * orientation.
 */
Support support_min(const Support& a, const Support& b);

/**
 * Add two Support values together, accounting for strand.
 */
Support operator+(const Support& one, const Support& other);

/**
 * Add in a Support to another.
 */
Support& operator+=(Support& one, const Support& other);

/**
 * Scale a Support by an integral factor.
 */
Support operator*(const Support& support, const size_t& scale);

/**
 * Scale a Support by an integral factor, the other way
 */
Support operator*(const size_t& scale, const Support& support);

    
}

#endif

