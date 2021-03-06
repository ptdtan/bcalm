/* remaining issue:
- no more than 2^32 sequences to glue together (should be ok for spruce)
*/
#include <gatb/gatb_core.hpp>
#include "unionFind.hpp"
#include "glue.hpp"
#include <atomic>
#include "../thirdparty/ThreadPool.h"
#include "../gatb-core/gatb-core/thirdparty/BooPHF/BooPHF.h"
#include <ctime> // for time
#include <iostream> // for time (and maybe other things?)

class bglue : public Tool
{
public:
    bglue() : Tool ("bglue"){
	getParser()->push_back (new OptionOneParam ("-k", "kmer size",  false,"31"));
	getParser()->push_back (new OptionOneParam ("-in", "input file",  true)); // necessary for repartitor
	getParser()->push_back (new OptionOneParam ("-out", "output file",  false, "out.fa"));
	getParser()->push_front (new OptionNoParam  ("--only-uf",   "(for debugging only) stop after UF construction", false));
	getParser()->push_front (new OptionNoParam  ("--uf-stats",   "display UF statistics", false));
	getParser()->push_back (new OptionOneParam ("--nb-glue-partitions", "number of glue files on disk",  false,"200"));
    };

    // Actual job done by the tool is here
    void execute ();
};

using namespace std;

const size_t SPAN = KMER_SPAN(1); // TODO: adapt span using Minia's technique
typedef Kmer<SPAN>::Type  Type;
typedef Kmer<SPAN>::Count Count;
typedef Kmer<SPAN>::ModelCanonical ModelCanon;
typedef Kmer<SPAN>::ModelMinimizer <ModelCanon> Model;
size_t kmerSize=31;
size_t minSize=8;
int nbGluePartitions = 200;
typedef uint64_t partition_t;
//typedef __uint128_t partition_t; // no

   // a hash wrapper for hashing kmers in Model form
    template <typename ModelType>
    class Hasher_T
    {
       public:
        ModelType model;

        Hasher_T(ModelType &model) : model(model) {};

        // fun fact: I tried with a mask = (1<<25)-1, 
        // and with chr14, it produced one big partition. So i guess that hash image needs to be large
        uint64_t operator ()  (const typename ModelType::Kmer& key, uint64_t seed = 0) const  {
                return model.getHash(key.value()) ;
                }
    };


 template <typename T>
void free_memory_vector(std::vector<T> &vec)
{
    vec.clear();
    vector<T>().swap(vec); // it's a trick to properly free the memory, as clear() doesn't cut it (http://stackoverflow.com/questions/3477715/c-vectorclear)
}


unsigned long logging(string message="")
{
    time_t t = time(0);   // get time now
    struct tm * now = localtime( & t );
    cout << setiosflags(ios::right);
    cout << resetiosflags(ios::left);
    cout << setw(40) << left << message << "      ";
    char tmp[128];
    snprintf (tmp, sizeof(tmp), "  %02d:%02d:%02d  ",
            now->tm_hour, now->tm_min, now->tm_sec);
    cout << tmp ;

    // using Progress.cpp of gatb-core
    u_int64_t mem = System::info().getMemorySelfUsed() / 1024;
    u_int64_t memMaxProcess = System::info().getMemorySelfMaxUsed() / 1024;
    snprintf (tmp, sizeof(tmp), "   memory [current, maxRSS]: [%4lu, %4lu] MB ",
            mem, memMaxProcess);

    cout << tmp << std::endl;
    return mem;
}

extern char revcomp (char s); // glue.cpp

string rc(string &s) {
	string rc;
	for (signed int i = s.length() - 1; i >= 0; i--) {rc += revcomp(s[i]);}
	return rc;
}


struct markedSeq
{
    string seq;
    bool lmark, rmark;
    string ks, ke; // [start,end] kmers of seq, in canonical form (redundant information with seq, but helpful)

    markedSeq(string seq, bool lmark, bool rmark, string ks, string ke) : seq(seq), lmark(lmark), rmark(rmark), ks(ks), ke(ke) {};

    void revcomp()
    {
        seq = rc(seq);
        std::swap(lmark, rmark);
        std::swap(ks, ke);
    }
};


// hack to refer to a sequences in msInPart as reverse complemented

uint32_t is_rev_index(uint32_t index)
{
    return ((index >> 31 & 1) == 1);
}


uint32_t rev_index(uint32_t index)
{
    if (is_rev_index(index))
    { std::cout << "Error: glue sequence index too large " << index << std::endl; exit(1);}
    return index | (1<<31);
}

uint32_t no_rev_index(uint32_t index)
{
    return index & ((1LL<<31) - 1LL);
}



vector<vector<uint32_t> > determine_order_sequences(vector<markedSeq> &sequences)
{
    bool debug = false ;
    unordered_map<string, set<uint32_t> > kmerIndex;
    set<uint32_t> usedSeq;
    vector<vector<uint32_t>> res;
    unsigned int nb_chained = 0;

    // index kmers to their seq
    for (uint32_t i = 0; i < sequences.size(); i++)
    {
        kmerIndex[sequences[i].ks].insert(i);
        kmerIndex[sequences[i].ke].insert(i);
    }

    for (unsigned int i = 0; i < sequences.size(); i++)
    {
        markedSeq current = sequences[i];
        if (usedSeq.find(i) != usedSeq.end())
                continue; // this sequence has already been glued

        if (current.lmark & current.rmark)
            continue; // not the extremity of a chain

        uint32_t chain_index = i;
        if (current.lmark)
        {
            current.revcomp(); // reverse so that lmark is false
            chain_index = rev_index(i);
        }

        assert(current.lmark == false);

        vector<uint32_t> chain;
        chain.push_back(chain_index);

        bool rmark = current.rmark;
        int current_index = i;
        int start_index = i;
        usedSeq.insert(i);

        while (rmark)
        {
            if (debug)
                std::cout << "current ke " << current.ke << " index " << current_index << " markings: " << current.lmark << current.rmark <<std::endl;

            // this sequence has a rmark, so necessarily there is another sequence to glue it with. find it here.
            set<uint32_t> candidateSuccessors = kmerIndex[current.ke];
           
            assert(candidateSuccessors.find(current_index) != candidateSuccessors.end()); // remove the current seq from our indexing data structure 
            candidateSuccessors.erase(current_index);

            assert(candidateSuccessors.size() == 1); // normally there is exactly one sequence to glue with

            int successor_index = *candidateSuccessors.begin(); // pop()
            assert(successor_index != current_index);
            markedSeq successor = sequences[successor_index];

            uint32_t chain_index = successor_index;

            if (successor.ks != current.ke || (!successor.lmark))
            {
                successor.revcomp();
                chain_index = rev_index(successor_index);
            }

            if (debug)
                std::cout << "successor " << successor_index << " successor ks ke "  << successor.ks << " "<< successor.ke << " markings: " << successor.lmark << successor.rmark << std::endl;

            assert(successor.lmark);
            assert(successor.ks == current.ke);

            // edge case where the seq to be glued starts and ends with itself. 
            // it should be a kmer (is tested below with an assert())
            if (successor.ks == successor.ke)
            {
                if (debug)
                    std::cout << "successor seq loops: " << successor.seq << std::endl;
                assert(successor.seq.size() == kmerSize);
                if (successor.lmark == false)
                    assert(successor.rmark == true);
                else
                    assert(successor.rmark == false);
                // it's the only possible cases I can think of

                // there is actually nothing to be done now, it's an extremity, so it will end.
                // on a side note, it's pointless to save this kmer in bcalm.
            }


            current = successor;
            chain.push_back(chain_index);
            current_index = successor_index;
            rmark = current.rmark;
            assert((usedSeq.find(current_index) == usedSeq.end()));
            usedSeq.insert(current_index);
        }

        res.push_back(chain);
        nb_chained += chain.size();
    }
    assert(sequences.size() == nb_chained); // make sure we've scheduled to glue all sequences in this partition
    return res;
}

/* straightforward glueing of a chain
 * sequences should be ordered and in the right orientation
 * so, it' just a matter of chopping of the first kmer
 */
string glue_sequences(vector<uint32_t> &chain, vector<markedSeq> &sequences)
{
    string res;
    string previous_kmer = "";
    unsigned int k = kmerSize;
    bool last_rmark = false;

    for (auto it = chain.begin(); it != chain.end(); it++)
    {

        uint32_t idx = *it;
       

        markedSeq ms = sequences[no_rev_index(idx)];

        if (is_rev_index(idx))
        {
            ms.revcomp();
        }
        
        string seq = ms.seq;

        if (previous_kmer.size() == 0) // it's the first element in a chain
        {
            assert(ms.lmark == false);
            res += seq;
        }
        else
        {
            assert(seq.substr(0, k).compare(previous_kmer) == 0);
            res += seq.substr(k);
        }

        previous_kmer = seq.substr(seq.size() - k);
        assert(previous_kmer.size() == k);
        last_rmark = ms.rmark;
    }
    assert(last_rmark == false);
    if (last_rmark) { cout<<"bad gluing, missed an element" << endl; exit(1); } // in case assert()'s are disabled

    return res;
}

// is also thread-safe thank to a lock
class BufferedFasta
{
        std::mutex mtx;
        std::vector<pair<string,string> > buffer;
//        std::vector<string > buffer;
        unsigned long buffer_length;

    public:
        BankFasta *bank;

        unsigned long max_buffer;

        BufferedFasta(string filename, unsigned long given_max_buffer = 500000)
        {
            max_buffer = given_max_buffer; // that much of buffering will be written to the file at once (in bytes)
            buffer_length = 0;
            bank = new BankFasta(filename);
        }

        ~BufferedFasta()
        {
            flush(); // probably very useful
            delete bank;
        }

        void insert(string &seq, string &comment)
        {
            mtx.lock();
            buffer_length += seq.size() + comment.size();
            buffer.push_back(make_pair(seq,comment));
//            buffer.push_back(seq);
           if (buffer_length > max_buffer)
                flush();
            mtx.unlock();
        }

        void flush()
        {
            for (auto &p : buffer)
            {
                string seq = get<0>(p);
                string comment = get<1>(p);
                Sequence s (Data::ASCII);
                s.getData().setRef ((char*)seq.c_str(), seq.size());
                s._comment = comment;
                bank->insert(s);
            }
            bank->flush();
            buffer_length = 0;
 //          std::cout << "buffer capacity" << buffer.capacity() << endl;
            buffer.clear();
            free_memory_vector(buffer);
        }
};

void output(string &seq, BufferedFasta &out, string comment = "")
{
    out.insert(seq, comment);
    // BufferedFasta takes care of the flush
}



 // used to get top N elements of a vector
template <typename T>
struct Comp{
    Comp( const vector<T>& v ) : _v(v) {}
    bool operator ()(T a, T b) { return _v[a] > _v[b]; }
    const vector<T>& _v;
};


//typedef boomphf::SingleHashFunctor<partition_t >  hasher_t;

// taken from GATB's MPHF.hpp and BooPHF.hpp (except that we don't need the iteration stuff from that file)
template<typename Key>
class hasher_t
{   
    typedef emphf::jenkins64_hasher BaseHasher;
    BaseHasher emphf_hasher;
    AdaptatorDefault<Key> adaptor;

    public:
    hasher_t(){
        std::mt19937_64 rng(37); // deterministic seed
        emphf_hasher = BaseHasher::generate(rng);
    }

    uint64_t operator ()  (const Key& key, uint64_t seed = 0) const  {
        if (seed != 0x33333333CCCCCCCCULL)
            return std::get<0>(emphf_hasher(adaptor(key)));
        return std::get<2>(emphf_hasher(adaptor(key)));
        // this is a big hack, because I'm lazy. 
        // I wanted to return two different hashes depending on how boophf calls it
        // since I contrl BooPHF code's, I know it calls this function with 0x33333333CCCCCCCCULL as the second seed.
    }
};

/* main */
void bglue::execute (){

    int nb_threads = getInput()->getInt("-nb-cores");
    std::cout << "Nb threads: " << nb_threads <<endl;
    kmerSize=getInput()->getInt("-k");
    nbGluePartitions=getInput()->getInt("--nb-glue-partitions");
    size_t k = kmerSize;
    string inputFile(getInput()->getStr("-in")); // necessary for repartitor

    string h5_prefix = inputFile.substr(0,inputFile.size()-2);
    IBank *in = Bank::open (h5_prefix + "glue");


    Storage* storage = StorageFactory(STORAGE_HDF5).load ( inputFile.c_str() );

    LOCAL (storage);
    /** We get the dsk and minimizers hash group in the storage object. */
    Group& dskGroup = storage->getGroup("dsk");
    Group& minimizersGroup = storage->getGroup("minimizers");

    typedef Kmer<SPAN>::Count Count;
    Partition<Count>& partition = dskGroup.getPartition<Count> ("solid");
    size_t nbPartitions = partition.size();
    cout << "DSK created " << nbPartitions << " partitions" << endl;

    /** We retrieve the minimizers distribution from the solid kmers storage. */
    Repartitor repart;
    repart.load (minimizersGroup);

    u_int64_t rg = ((u_int64_t)1 << (2*minSize));

    /* Retrieve frequency of minimizers;
     * actually only used in minimizerMin and minimizerMax */
    uint32_t *freq_order = NULL;

    int minimizer_type = 1; // typical bcalm usage.
    if (minimizer_type == 1)
    {
        freq_order = new uint32_t[rg];
        Storage::istream is (minimizersGroup, "minimFrequency");
        is.read ((char*)freq_order, sizeof(uint32_t) * rg);
    }

#if 0  // all those models are for creating UF with k-1 mers or minimizers, we don't do that anymore. legacy/debugging code, that can be removed later.
    Model model(kmerSize, minSize, Kmer<SPAN>::ComparatorMinimizerFrequencyOrLex(), freq_order);
    Model modelK1(kmerSize-1, minSize,  Kmer<SPAN>::ComparatorMinimizerFrequencyOrLex(), freq_order);
    Model modelK2(kmerSize-2, minSize,  Kmer<SPAN>::ComparatorMinimizerFrequencyOrLex(), freq_order);
    Model modelM(minSize, minSize,  Kmer<SPAN>::ComparatorMinimizerFrequencyOrLex(), freq_order);
#endif

    // create a hasher for UF
    ModelCanon modelCanon(kmerSize); // i'm a bit lost with those models.. I think GATB could be made more simple here.
    Hasher_T<ModelCanon> hasher(modelCanon);


    Iterator<Sequence>* it = in->iterator();

    int nb_uf_hashes_vectors = 1000;
    std::vector<std::vector<partition_t >> uf_hashes_vectors(nb_uf_hashes_vectors);
    // std::mutex uf_hashes_vectorsMutex[nb_uf_hashes_vectors];
    std::mutex *uf_hashes_vectorsMutex=new std::mutex  [nb_uf_hashes_vectors];

    // prepare UF: create the set of keys
    auto prepareUF = [k, &modelCanon, \
        &uf_hashes_vectorsMutex, &uf_hashes_vectors, &hasher, nb_uf_hashes_vectors](const Sequence& sequence)
    {
        string seq = sequence.toString();
        string comment = sequence.getComment();

        bool lmark = comment[0] == '1';
        bool rmark = comment[1] == '1';

        if ((!lmark) && (!rmark)) // if both marks are 0, nothing to glue here
            return;

        string kmerBegin = seq.substr(0, k );
        string kmerEnd = seq.substr(seq.size() - k , k );

        // UF of canonical kmers in ModelCanon form, then hashed
        ModelCanon::Kmer kmmerBegin = modelCanon.codeSeed(kmerBegin.c_str(), Data::ASCII);
        ModelCanon::Kmer kmmerEnd = modelCanon.codeSeed(kmerEnd.c_str(), Data::ASCII);

        uint64_t h1 = hasher(kmmerBegin);
        uint64_t h2 = hasher(kmmerEnd);

        uf_hashes_vectorsMutex[h1%nb_uf_hashes_vectors].lock();
        uf_hashes_vectors[h1%nb_uf_hashes_vectors].push_back(h1);
        uf_hashes_vectorsMutex[h1%nb_uf_hashes_vectors].unlock();

        uf_hashes_vectorsMutex[h2%nb_uf_hashes_vectors].lock();
        uf_hashes_vectors[h2%nb_uf_hashes_vectors].push_back(h2);
        uf_hashes_vectorsMutex[h2%nb_uf_hashes_vectors].unlock();
    };

    setDispatcher (  new Dispatcher (getInput()->getInt(STR_NB_CORES)) );
    it = in->iterator(); // yeah so.. I think the old iterator cannot be reused
    getDispatcher()->iterate (it, prepareUF);

    logging("created vector of redundant UF elements");

    ThreadPool uf_merge_pool(nb_threads);

    // uniquify UF vectors
    for (int i = 0; i < nb_uf_hashes_vectors; i++)
    {

        auto uniquify = [&uf_hashes_vectors, i] (int thread_id)
        {
            std::vector<partition_t> &vec = uf_hashes_vectors[i];
            //http://stackoverflow.com/questions/1041620/whats-the-most-efficient-way-to-erase-duplicates-and-sort-a-vector
            set<partition_t> s( vec.begin(), vec.end() );
            vec.assign( s.begin(), s.end() );

        };
        uf_merge_pool.enqueue(uniquify);
    }

    uf_merge_pool.join();

    logging("sorted and unique UF elements");

    // compute number of UF elements from intermediate vectors
    unsigned long tmp_nb_uf_keys = 0;
    for (int i = 0; i < nb_uf_hashes_vectors; i++)
        tmp_nb_uf_keys += uf_hashes_vectors[i].size();

    // merge intermediate vectors into a single vector, to prepare MPHF (this step could be skipped if created a special iterator for boophf)
    std::vector<partition_t > uf_hashes;
    uf_hashes.reserve(tmp_nb_uf_keys);
    for (int i = 0; i < nb_uf_hashes_vectors; i++)
    {
        uf_hashes.insert( uf_hashes.end(), uf_hashes_vectors[i].begin(), uf_hashes_vectors[i].end());
        free_memory_vector(uf_hashes_vectors[i]);
    }

    logging("merged UF elements (" + std::to_string(uf_hashes.size()) + ") into a single vector");

    unsigned long nb_uf_keys = uf_hashes.size();
    if (nb_uf_keys != tmp_nb_uf_keys) { std::cout << "Error during UF preparation, bad number of keys in merge: " << tmp_nb_uf_keys << " " << nb_uf_keys << std::endl; exit(1); }

	auto data_iterator = boomphf::range(uf_hashes.begin(), uf_hashes.end());

    boomphf::mphf<partition_t , hasher_t< partition_t> > uf_mphf(nb_uf_keys, data_iterator, nb_threads);

    free_memory_vector(uf_hashes);

    unsigned long uf_mphf_memory = uf_mphf.totalBitSize();
    logging("UF MPHF constructed (" + std::to_string(uf_mphf_memory/8/1024/1024) + " MB)" );


    // create a UF data structure
#if 0
    unionFind<unsigned int> ufmin;
    unionFind<std::string> ufprefixes;
    unsigned int prefix_length = 10;
    unionFind<std::string> ufkmerstr;
#endif
    // those were toy one, here is the real one:
    unionFind<uint32_t> ufkmers(nb_uf_keys);
    // instead of UF of kmers, we do a union find of hashes of kmers. less memory. will have collisions, but that's okay i think. let's see.
    // actually, in the current implementation, partition_t is not used, but values are indeed hardcoded in 32 bits (the UF implementation uses a 64 bits hash table for internal stuff)

    // We loop over sequences.
    /*for (it.first(); !it.isDone(); it.next())
    {
        string seq = it->toString();*/
    auto createUF = [k, &modelCanon, \
        &uf_mphf, &ufkmers, &hasher](const Sequence& sequence)
    {
        string seq = sequence.toString();

        if (seq.size() < k)
        {
            std::cout << "unexpectedly small sequence found ("<<seq.size()<<"). did you set k correctly?" <<std::endl; exit(1);
        }

        string comment = sequence.getComment();
        bool lmark = comment[0] == '1';
        bool rmark = comment[1] == '1';

        if ((!lmark) && (!rmark)) // if both marks are 0, nothing to glue here
            return;

        string kmerBegin = seq.substr(0, k );
        string kmerEnd = seq.substr(seq.size() - k , k );

        // UF of canonical kmers in ModelCanon form, then hashed
        ModelCanon::Kmer kmmerBegin = modelCanon.codeSeed(kmerBegin.c_str(), Data::ASCII);
        ModelCanon::Kmer kmmerEnd = modelCanon.codeSeed(kmerEnd.c_str(), Data::ASCII);

        ufkmers.union_(uf_mphf.lookup(hasher(kmmerBegin)), uf_mphf.lookup(hasher(kmmerEnd)));
        //ufkmers.union_((hasher(kmmerBegin)), (hasher(kmmerEnd)));

#if 0

        Model::Kmer kmmerBegin = model.codeSeed(kmerBegin.c_str(), Data::ASCII);
        Model::Kmer kmmerEnd = model.codeSeed(kmerEnd.c_str(), Data::ASCII);

        // UF of canonical kmers in string form, not hashed
        string canonicalKmerBegin = modelK1.toString(kmmerBegin.value());
        string canonicalKmerEnd = modelK1.toString(kmmerEnd.value());
        ufkmerstr.union_(canonicalKmerBegin, canonicalKmerEnd);

        // UF of minimizers of kmers
        size_t leftMin(modelK1.getMinimizerValue(kmmerBegin.value()));
        size_t rightMin(modelK1.getMinimizerValue(kmmerEnd.value()));
        ufmin.union_(leftMin, rightMin);

        // UF of prefix of kmers in string form
        string prefixCanonicalKmerBegin = canonicalKmerBegin.substr(0, prefix_length);
        string prefixCanonicalKmerEnd = canonicalKmerEnd.substr(0, prefix_length);
        ufprefixes.union_(prefixCanonicalKmerBegin, prefixCanonicalKmerEnd);
#endif


    };

    //setDispatcher (new SerialDispatcher()); // force single thread
    setDispatcher (  new Dispatcher (nb_threads) );
    it = in->iterator(); // yeah so.. I think the old iterator cannot be reused
    getDispatcher()->iterate (it, createUF);

#if 0
    ufmin.printStats("uf minimizers");

    ufprefixes.printStats("uf " + to_string(prefix_length) + "-prefixes of kmers");

    ufkmerstr.printStats("uf kmers, std::string");
#endif


    unsigned long memUF = logging("UF constructed");

    if (getParser()->saw("--uf-stats")) // for debugging
    {
        ufkmers.printStats("uf kmers");
        unsigned long memUFpostStats = logging("after computing UF stats");
    }

    if (getParser()->saw("--only-uf")) // for debugging
        return;

    /* now we're mirroring the UF to a vector of uint32_t's, it will take less space, and strictly same information
     * this is to get rid of the rank (one uint32) per element in the current UF implementation */

    std::vector<uint32_t > ufkmers_vector(nb_uf_keys);
    for (unsigned long i = 0; i < nb_uf_keys; i++)
        ufkmers_vector[i] = ufkmers.find(i);

    logging("UF to vector done");
    
    free_memory_vector(ufkmers.mData);

    logging("freed original UF");


    // setup output file
    string output_prefix = getInput()->getStr("-out");
    std::atomic<unsigned long> out_id; // identified for output sequences
    out_id = 0;
    BufferedFasta out (output_prefix, 4000000 /* give it a large buffer*/);
    out.bank->setDataLineSize(0); // antoine wants one seq per line in output

    auto get_partition = [&modelCanon, &ufkmers_vector, &hasher, &uf_mphf]
        (string &kmerBegin, string &kmerEnd,
         bool lmark, bool rmark,
         ModelCanon::Kmer &kmmerBegin, ModelCanon::Kmer &kmmerEnd,  // those will be populated based on lmark and rmark
         bool &found_partition)
        {
            found_partition = false;
            uint32_t partition = 0;

            if (lmark)
            {
                kmmerBegin = modelCanon.codeSeed(kmerBegin.c_str(), Data::ASCII);
                found_partition = true;
                partition = ufkmers_vector[uf_mphf.lookup(hasher(kmmerBegin))];
            }

            if (rmark)
            {
                kmmerEnd = modelCanon.codeSeed(kmerEnd.c_str(), Data::ASCII);

                if (found_partition) // just do a small check
                {
                    if (ufkmers_vector[uf_mphf.lookup(hasher(kmmerEnd))] != partition)
                    { std::cout << "bad UF! left kmer has partition " << partition << " but right kmer has partition " << ufkmers_vector[uf_mphf.lookup(hasher(kmmerEnd))] << std::endl; exit(1); }
                }
                else
                {
                    partition = ufkmers_vector[uf_mphf.lookup(hasher(kmmerEnd))];
                    found_partition = true;
                }
            }

            return partition;
        };

    // std::mutex gluePartitionsLock[nbGluePartitions];
    std::mutex *gluePartitionsLock=new     std::mutex [nbGluePartitions];
    std::mutex outLock; // for the main output file
    std::vector<BufferedFasta*> gluePartitions(nbGluePartitions);
    std::string gluePartition_prefix = output_prefix + ".gluePartition.";
    unsigned int max_buffer = 50000;
    std::vector<std::atomic<unsigned long>> nb_seqs_in_partition(nbGluePartitions);


    for (int i = 0; i < nbGluePartitions; i++)
    {
        gluePartitions[i] = new BufferedFasta(gluePartition_prefix + std::to_string(i), max_buffer);
        nb_seqs_in_partition[i] = 0;
    }

    logging( "Allowed " + to_string((max_buffer * nbGluePartitions) /1024 /1024) + " MB memory for buffers");

    // partition the glue into many files, à la dsk
    auto partitionGlue = [k, &modelCanon /* crashes if copied!*/, \
        &get_partition, &gluePartitions, &gluePartitionsLock,
        &out, &outLock, &nb_seqs_in_partition, &out_id]
            (const Sequence& sequence)
    {
        string seq = sequence.toString();

        string comment = sequence.getComment();
        bool lmark = comment[0] == '1';
        bool rmark = comment[1] == '1';

        string kmerBegin = seq.substr(0, k );
        string kmerEnd = seq.substr(seq.size() - k , k );

        // make canonical kmer
        ModelCanon::Kmer kmmerBegin;
        ModelCanon::Kmer kmmerEnd;

        bool found_partition = false;

        uint32_t partition = get_partition(kmerBegin, kmerEnd, lmark, rmark, kmmerBegin, kmmerEnd, found_partition);

        // compute kmer extremities if we have not already
        if (!lmark)
            kmmerBegin = modelCanon.codeSeed(kmerBegin.c_str(), Data::ASCII);
        if (!rmark)
            kmmerEnd = modelCanon.codeSeed(kmerEnd.c_str(), Data::ASCII);

        if (!found_partition) // this one doesn't need to be glued
        {
            output(seq, out, std::to_string(out_id++)); // maybe could optimize writing by using queues
            return;
        }

        int index = partition % nbGluePartitions;
        //stringstream ss1; // to save partition later in the comment
        //ss1 << blabla;

        output(seq, *gluePartitions[index], comment);
        nb_seqs_in_partition[index]++;
    };

    logging("Disk partitioning of glue");

    setDispatcher (  new Dispatcher (getInput()->getInt(STR_NB_CORES)) );
    it = in->iterator(); // yeah so.. I think the old iterator cannot be reused
    getDispatcher()->iterate (it, partitionGlue);

    // get top10 largest glue partitions
    int top_n_glue_partition = 10;
    vector<unsigned long> vx, copy_nb_seqs_in_partition;
    vx.resize(nb_seqs_in_partition.size());
    copy_nb_seqs_in_partition.resize(nb_seqs_in_partition.size());
    for(unsigned int i= 0; i<nb_seqs_in_partition.size(); ++i ) 
    {
        vx[i]= i;
        copy_nb_seqs_in_partition[i] = nb_seqs_in_partition[i]; // to get rid of atomic type
    }
    partial_sort( vx.begin(), vx.begin()+top_n_glue_partition, vx.end(), Comp<unsigned long>(copy_nb_seqs_in_partition) );

    std::cout << "Top 10 glue partitions by size:" << std::endl;
    for (int i = 0; i < top_n_glue_partition; i++)
        std::cout << "Glue partition " << vx[i] << " has " << copy_nb_seqs_in_partition[vx[i]] << " sequences " << endl;

    for (int i = 0; i < nbGluePartitions; i++)
    {
        delete gluePartitions[i]; // takes care of the final flush (this doesn't delete the file, just closes it)
    }

    out.flush();


    logging("Glueing partitions");

    // glue all partitions using a thread pool
    ThreadPool pool(nb_threads);
    for (int partition = 0; partition < nbGluePartitions; partition++)
    {
        auto glue_partition = [&modelCanon, &ufkmers, &hasher, partition, &gluePartition_prefix,
        &get_partition, &out, &outLock, &out_id]( int thread_id)
        {
            int k = kmerSize;

            string partitionFile = gluePartition_prefix + std::to_string(partition);
            BankFasta partitionBank (partitionFile);

            outLock.lock(); // should use a printlock..
            string message = "Gluing partition " +to_string(partition) + " (size: " +to_string(System::file().getSize(partitionFile)/1024/1024) + " MB)";
            logging(message);
            outLock.unlock();

            BankFasta::Iterator it (partitionBank);

            unordered_map<int,vector<markedSeq>> msInPart;

            for (it.first(); !it.isDone(); it.next())
            {
                string seq = it->toString();

                string kmerBegin = seq.substr(0, k );
                string kmerEnd = seq.substr(seq.size() - k , k );

                uint32_t partition = 0;
                bool found_partition = false;

                string comment = it->getComment();
                bool lmark = comment[0] == '1';
                bool rmark = comment[1] == '1';

                // todo speed improvement: get partition id from sequence header (so, save it previously)

                // make canonical kmer
                ModelCanon::Kmer kmmerBegin;
                ModelCanon::Kmer kmmerEnd;

                partition = get_partition(kmerBegin, kmerEnd, lmark, rmark, kmmerBegin, kmmerEnd, found_partition);

                // compute kmer extremities if we have not already
                if (!lmark)
                    kmmerBegin = modelCanon.codeSeed(kmerBegin.c_str(), Data::ASCII);
                if (!rmark)
                    kmmerEnd = modelCanon.codeSeed(kmerEnd.c_str(), Data::ASCII);

                string ks = modelCanon.toString(kmmerBegin.value());
                string ke = modelCanon.toString(kmmerEnd  .value());
                markedSeq ms(seq, lmark, rmark, ks, ke);

                msInPart[partition].push_back(ms);
            }


            // now iterates all sequences in a partition to glue them in clever order (avoid intermediate gluing)
            for (auto it = msInPart.begin(); it != msInPart.end(); it++)
            {
                //std::cout << "1.processing partition " << it->first << std::endl;
                vector<vector<uint32_t>> ordered_sequences_idxs = determine_order_sequences(it->second); // return indices of markedSeq's inside it->second
                //std::cout << "2.processing partition " << it->first << " nb ordered sequences: " << ordered_sequences.size() << std::endl;

                for (auto itO = ordered_sequences_idxs.begin(); itO != ordered_sequences_idxs.end(); itO++)
                {
                    string seq = glue_sequences(*itO, it->second); // takes as input the indices of ordered sequences, and the markedSeq's themselves

                    output(seq, out, std::to_string(out_id++));
                }

                free_memory_vector(it->second);
            }

            partitionBank.finalize();

            System::file().remove (partitionFile);

        };

        pool.enqueue(glue_partition);
    }

    pool.join();

    logging("end");


//#define ORIGINAL_GLUE
#ifdef ORIGINAL_GLUE
    // We loop again over sequences
    // but this time we glue!
    int nb_glues = 1;
    BankFasta out (getInput()->getStr("-out") + ".original_glue_version");
    GlueCommander glue_commander(kmerSize, &out, nb_glues, &model);
    for (it.first(); !it.isDone(); it.next())
    {
        string seq = it->toString();
        string comment = it->getComment();
        bool lmark = comment[0] == '1';
        bool rmark = comment[1] == '1';
        GlueEntry e(seq, lmark, rmark, kmerSize);
        glue_commander.insert(e);
    }

    glue_commander.stop();
    cout << "Final glue:\n";
    glue_commander.dump();
    cout << "*****\n";
    glue_commander.printMemStats();
#endif

}

int main (int argc, char* argv[])
{
    try
    {
        bglue().run (argc, argv);
    }
    catch (Exception& e)
    {
        std::cout << "EXCEPTION: " << e.getMessage() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
