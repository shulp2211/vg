/** \file paths_main.cpp
 *
 * Defines the "vg paths" subcommand, which reads paths in the graph.
 */


#include <omp.h>
#include <unistd.h>
#include <getopt.h>

#include <algorithm>
#include <iostream>

#include "subcommand.hpp"

#include "../vg.hpp"
#include "../xg.hpp"
#include "../gbwt_helper.hpp"
#include <vg/io/vpkg.hpp>
#include <vg/io/stream.hpp>

using namespace std;
using namespace vg;
using namespace vg::subcommand;

void help_paths(char** argv) {
    cerr << "usage: " << argv[0] << " paths [options]" << endl
         << "options:" << endl
         << "  input:" << endl
         << "    -v, --vg FILE         use the paths in this vg FILE" << endl
         << "    -x, --xg FILE         use the paths in the XG index FILE" << endl
         << "    -g, --gbwt FILE       use the threads in the GBWT index in FILE" << endl
         << "                          (XG index or vg graph required for most output options; -g takes priority over -x)" << endl
         << "  output:" << endl
         << "    -X, --extract-gam     print (as GAM alignments) the stored paths in the graph" << endl
         << "    -V, --extract-vg      print (as path-only .vg) the queried paths" << endl
         << "    -L, --list            print (as a list of names, one per line) the path (or thread) names" << endl
         << "    -E, --lengths         print a list of path names (as with -L) but paired with their lengths" << endl
         << "    -F, --extract-fasta   print the paths in FASTA format" << endl
         << "  path selection:" << endl
         << "    -Q, --paths-by STR    select the paths with the given name prefix" << endl
         << "    -S, --sample STR      select the threads for this sample (GBWT)" << endl;
}

int main_paths(int argc, char** argv) {

    if (argc == 2) {
        help_paths(argv);
        return 1;
    }

    bool extract_as_gam = false;
    bool extract_as_vg = false;
    bool list_names = false;
    bool extract_as_fasta = false;
    string xg_file;
    string vg_file;
    string gbwt_file;
    string path_prefix;
    string sample_name;
    bool list_lengths = false;
    size_t output_formats = 0, selection_criteria = 0;

    int c;
    optind = 2; // force optind past command positional argument
    while (true) {
        static struct option long_options[] =

        {
            {"vg", required_argument, 0, 'v'},
            {"xg", required_argument, 0, 'x'},
            {"gbwt", required_argument, 0, 'g'},
            {"extract-gam", no_argument, 0, 'X'},
            {"extract-vg", no_argument, 0, 'V'},
            {"list", no_argument, 0, 'L'},
            {"lengths", no_argument, 0, 'E'},
            {"extract-fasta", no_argument, 0, 'F'},
            {"paths-by", required_argument, 0, 'Q'},
            {"sample", required_argument, 0, 'S'},

            // Hidden options for backward compatibility.
            {"threads", no_argument, 0, 'T'},
            {"threads-by", required_argument, 0, 'q'},

            {0, 0, 0, 0}
        };

        int option_index = 0;
        c = getopt_long (argc, argv, "hLXv:x:g:Q:VEFS:Tq:",
                long_options, &option_index);

        // Detect the end of the options.
        if (c == -1)
            break;

        switch (c)
        {

        case 'v':
            vg_file = optarg;
            break;

        case 'x':
            xg_file = optarg;
            break;

        case 'g':
            gbwt_file = optarg;
            break;
            
        case 'X':
            extract_as_gam = true;
            output_formats++;
            break;

        case 'V':
            extract_as_vg = true;
            output_formats++;
            break;

        case 'L':
            list_names = true;
            output_formats++;
            break;

        case 'E':
            list_names = true;
            list_lengths = true;
            output_formats++;
            break;

        case 'Q':
            path_prefix = optarg;
            selection_criteria++;
            break;

        case 'S':
            sample_name = optarg;
            selection_criteria++;
            break;

        case 'F':
            extract_as_fasta = true;
            output_formats++;
            break;

        case 'T':
            std::cerr << "warning: [vg paths] option --threads is obsolete and unnecessary" << std::endl;
            break;

        case 'q':
            std::cerr << "warning: [vg paths] option --threads-by is deprecated; please use --paths-by" << std::endl;
            path_prefix = optarg;
            selection_criteria++;
            break;

        case 'h':
        case '?':
            help_paths(argv);
            exit(1);
            break;

        default:
            abort ();
        }
    }

    if (!vg_file.empty() && !(xg_file.empty() && gbwt_file.empty())) {
        std::cerr << "error: [vg paths] cannot read input from multiple sources" << std::endl;
        std::exit(EXIT_FAILURE);
    }
    if (!gbwt_file.empty()) {
        bool need_graph = (extract_as_gam || extract_as_vg || list_lengths || extract_as_fasta);
        if (need_graph && xg_file.empty() && vg_file.empty()) {
            std::cerr << "error: [vg paths] an XG index or vg graph needed for extracting threads in -X, -V, -E or -F format" << std::endl;
            std::exit(EXIT_FAILURE);
        }
        if (!need_graph && (!xg_file.empty() || !vg_file.empty())) {
            // TODO: This should be an error, but we display a warning instead for backward compatibility.
            //std::cerr << "error: [vg paths] cannot read input from multiple sources" << std::endl;
            //std::exit(EXIT_FAILURE);
            std::cerr << "warning: [vg paths] XG index and/or vg graph  unnecessary for listing GBWT threads" << std::endl;
        }
    }
    if (output_formats != 1) {
        std::cerr << "error: [vg paths] one output format (-X, -V, -L, or -E) must be specified" << std::endl;
        std::exit(EXIT_FAILURE);
    }
    if (selection_criteria > 1) {
        std::cerr << "error: [vg paths] multiple selection criteria (-Q, -S) cannot be used" << std::endl;
        std::exit(EXIT_FAILURE);
    }
    if (!sample_name.empty() && gbwt_file.empty()) {
        std::cerr << "error: [vg paths] selection by sample name only works with a GBWT index" << std::endl;
        std::exit(EXIT_FAILURE);
    }
    
    // Load whatever indexes we were given
    // Note: during handlifiction, distinction between -v and -x options disappeared.
    unique_ptr<PathHandleGraph> graph;
    if (!vg_file.empty() || !xg_file.empty()) {
        assert(vg_file.empty() || xg_file.empty());
        const string& graph_file = vg_file.empty() ? xg_file : vg_file;
        // Load the vg or xg
        get_input_file(graph_file, [&](istream& in) {
                graph = vg::io::VPKG::load_one<PathHandleGraph>(in);
            });
    }
    unique_ptr<gbwt::GBWT> gbwt_index;
    if (!gbwt_file.empty()) {
        // We want a gbwt
        
        // Load the GBWT from its container
        gbwt_index = vg::io::VPKG::load_one<gbwt::GBWT>(gbwt_file);

        if (gbwt_index.get() == nullptr) {
          // Complain if we couldn't.
          cerr << "error: [vg paths] unable to load gbwt index file" << endl;
          exit(1);
        }
    }

    // We may need to emit a stream of Alignemnts
    unique_ptr<vg::io::ProtobufEmitter<Alignment>> gam_emitter;
    // Or we might need to emit a stream of VG Graph objects
    unique_ptr<vg::io::ProtobufEmitter<Graph>> graph_emitter;
    if (extract_as_gam) {
        // Open up a GAM output stream
        gam_emitter = unique_ptr<vg::io::ProtobufEmitter<Alignment>>(new vg::io::ProtobufEmitter<Alignment>(cout));
    } else if (extract_as_vg) {
        // Open up a VG Graph chunk output stream
        graph_emitter = unique_ptr<vg::io::ProtobufEmitter<Graph>>(new vg::io::ProtobufEmitter<Graph>(cout));
    }
    
    if (gbwt_index.get() != nullptr) {

        if (!(gbwt_index->hasMetadata() && gbwt_index->metadata.hasPathNames())) {
            std::cerr << "warning: [vg paths] the GBWT index does not contain thread names" << std::endl;
            std::exit(EXIT_SUCCESS);
        }

        // Select the threads we are interested in.
        std::vector<gbwt::size_type> thread_ids;
        if (!sample_name.empty()) {
            if (!gbwt_index->metadata.hasSampleNames()) {
                std::cerr << "error: [vg paths] the GBWT index does not contain sample names" << std::endl;
                std::exit(EXIT_FAILURE);
            }
            gbwt::size_type sample_id = gbwt_index->metadata.sample(sample_name);
            if (sample_id < gbwt_index->metadata.samples()) {
                thread_ids = gbwt_index->metadata.pathsForSample(sample_id);
            }
        } else if(!path_prefix.empty()) {
            for (size_t i = 0; i < gbwt_index->metadata.paths(); i++) {
                std::string name = thread_name(*gbwt_index, i);
                if (name.length() >= path_prefix.length() && std::equal(path_prefix.begin(), path_prefix.end(), name.begin())) {
                    thread_ids.push_back(i);
                }
            }
        } else {
            thread_ids.reserve(gbwt_index->metadata.paths());
            for (size_t i = 0; i < gbwt_index->metadata.paths(); i++) {
                thread_ids.push_back(i);
            }
        }
        
        // Process the threads.
        for (gbwt::size_type id : thread_ids) {
            std::string name = thread_name(*gbwt_index, id);        

            // We are only interested in the name
            if (list_names && !list_lengths) {
                std::cout << name << endl;
                continue;
            }
            
            // Otherwise we need the actual thread data
            gbwt::vector_type sequence = gbwt_index->extract(gbwt::Path::encode(id, false));
            Path path;
            path.set_name(name);
            size_t rank = 1;
            for (auto node : sequence) {
                // Put each node in the constructed path
                Mapping* m = path.add_mapping();
                Position* p = m->mutable_position();
                p->set_node_id(gbwt::Node::id(node));
                p->set_is_reverse(gbwt::Node::is_reverse(node));
                Edit* e = m->add_edit();
                size_t len = graph->get_length(graph->get_handle(p->node_id()));
                e->set_to_length(len);
                e->set_from_length(len);
                m->set_rank(rank++);
            }
            if (extract_as_gam) {
                // Write as an Alignment
                gam_emitter->write(alignment_from_path(*graph, path));
            } else if (extract_as_vg) {
                // Write as a Path in a VG
                Graph g;
                *(g.add_path()) = path;
                graph_emitter->write(std::move(g));
            } else if (extract_as_fasta) {
                write_fasta_sequence(name, path_sequence(*graph, path), cout);
            }
            if (list_lengths) {
                cout << path.name() << "\t" << path_to_length(path) << endl;
            }
        }
    } else if (graph.get() != nullptr) {
        // Handle non-thread queries from vg
        
        auto check_prefix = [&path_prefix](const string& name) {
            // Note: we're using a filter rather than index, so O(#paths in graph). 
            return path_prefix.empty() ||
            std::mismatch(name.begin(), name.end(),
                          path_prefix.begin(), path_prefix.end()).second == path_prefix.end();
        };

        graph->for_each_path_handle([&](path_handle_t path_handle) {
                string path_name = graph->get_path_name(path_handle);
                if (check_prefix(path_name)) {
                    if (list_names) {
                        cout << path_name;
                        if (list_lengths) {
                            size_t path_length = 0;
                            for (handle_t handle : graph->scan_path(path_handle)) {
                                path_length += graph->get_length(handle);
                            }
                            cout << "\t" << path_length;
                        }
                    } else {
                        Path path = path_from_path_handle(*graph, path_handle);
                        if (extract_as_gam) {
                            gam_emitter->write(alignment_from_path(*graph, path));
                        } else if (extract_as_vg) {
                            Graph g;
                            *(g.add_path()) = path;
                            graph_emitter->write(std::move(g));
                        } else if (extract_as_fasta) {
                            write_fasta_sequence(path_name, path_sequence(*graph, path), cout);
                        }
                    }
                }
            });
    }
    return 0;

}

// Register subcommand
static Subcommand vg_paths("paths", "traverse paths in the graph", main_paths);

