#include "stb/stb_image_write.h"

#include "nanosvg/nanosvg.h"
#include "nanosvg/nanosvgrast.h"

#include <memory>
#include <iostream>
#include <fstream>
#include <vector>
#include <queue>
#include <unordered_map>
#include <string>
#include <cstring>
#include <thread>
#include <semaphore>
#include <mutex>
#include <string>
#include <filesystem>
#include <atomic>

namespace fs = std::filesystem;

namespace gif643 {

const size_t    BPP         = 4;    // Bytes per pixel
const float     ORG_WIDTH   = 48.0; // Original SVG image width in px.
const int       NUM_THREADS = 1;    // Default value, changed by argv. 
std::binary_semaphore take_from_queu_signal{1};
std::counting_semaphore queu_updated_signal{0};
std::mutex cache_mutex_;
std::mutex queu_mutex;

using PNGDataVec = std::vector<char>;
using PNGDataPtr = std::shared_ptr<PNGDataVec>;

/// \brief Wraps callbacks from stbi_image_write
//
// Provides a static method to give to stbi_write_png_to_func (rawCallback),
// which will call with a void* pointer to the instance of PNGWriter.
// The actual processing should occur in callback(...).
//
// Usage (see stbi_write_png for w,h, BPP, image_data and stride parameters): 
//
//   PNGWriter writer;
//   writer(w, h, BPP, image_data, stride); // Will ultimately 
//                                          // call callback(data, len) and
//                                          // return when it's done.
//                                          // Throws if an error occured.
//
class PNGWriter
{
private:
    PNGDataPtr png_data_;

public:
    static void rawCallback(void* context, void* data, int len)
    {
        PNGWriter* writer = (PNGWriter*)context;
        writer->callback(data, len);
    }

    /// \brief Copies the PNG data in png_data_ when compression is done and
    ///        ready to write.
    ///
    /// Meant to be called by stbi_image_write_to_func as callback.
    /// NOTE: The given data array is deleted by the caller once callback is 
    /// finished.
    /// \param data Raw pointer to uint8 array.
    /// \param len  size of the data array.
    void callback(void* data, size_t len)
    {
        char* data_raw = static_cast<char *>(data);
        png_data_ = PNGDataPtr(new PNGDataVec(&data_raw[0], &data_raw[len]));
    }

    void operator()(size_t width,
                    size_t height,
                    size_t BPP,
                    const unsigned char* image_data,
                    size_t stride)
    {
            stbi_write_func* fun = PNGWriter::rawCallback;
            int r = stbi_write_png_to_func(fun, 
                                           this,
                                           width,
                                           height,
                                           BPP,
                                           &image_data[0],
                                           stride);

            if (r == 0) {
                throw std::runtime_error("Error in write_png_to_func");
            }
    }

    /// \brief Return a shared pointer to the compressed PNG data.
    PNGDataPtr getData()
    {
        return png_data_;
    }
};

/// \brief Task definition
///
/// fname_in:  The file to process (SVG format)
/// fname_out: Where to write the result in PNG.
/// size:      The size, in pixel, of the produced image.
///
/// NOTE: Assumes the input SVG is ORG_WIDTH wide (48px) and the result will be
/// square. Does not matter if it does not fit in the resulting image, it will //// simply be cropped.
struct TaskDef
{
    std::string fname_in;
    std::string fname_out; 
    int size;
};

/// \brief A class representing the processing of one SVG file to a PNG stream.
///
/// Not thread safe !
///
class TaskRunner
{
private:
    TaskDef task_def_;

public:
    TaskRunner(const TaskDef& task_def):
        task_def_(task_def)
    {
    }

    void operator()()
    {
        const std::string&  fname_in    = task_def_.fname_in;
        const std::string&  fname_out   = task_def_.fname_out;
        const size_t&       width       = task_def_.size; 
        const size_t&       height      = task_def_.size; 
        const size_t        stride      = width * BPP;
        const size_t        image_size  = height * stride;
        const float&        scale       = float(width) / ORG_WIDTH;

        std::cerr << "Running for "
                  << fname_in 
                  << "..." 
                  << std::endl;

        NSVGimage*          image_in        = nullptr;
        NSVGrasterizer*     rast            = nullptr;

    
        
    
        try {

            // Read the file ...
            image_in = nsvgParseFromFile(fname_in.c_str(), "px", 0);
            if (image_in == nullptr) {
                std::string msg = "Cannot parse '" + fname_in + "'.";
                throw std::runtime_error(msg.c_str());
            }
            // Raster it ...
            std::vector<unsigned char> image_data(image_size, 0);
            rast = nsvgCreateRasterizer();
            nsvgRasterize(rast,
                          image_in,
                          0,
                          0,
                          scale,
                          &image_data[0],
                          width,
                          height,
                          stride);

            // Compress it ...
            PNGWriter writer;
            writer(width, height, BPP, &image_data[0], stride);
            // Write it out ...
            std::ofstream file_out(fname_out, std::ofstream::binary);
            auto data = writer.getData();
            file_out.write(&(data->front()), data->size());
            
        } catch (std::runtime_error e) {
            std::cerr << "Exception while processing "
                      << fname_in
                      << ": "
                      << e.what()
                      << std::endl;
        }
        
        // Bring down ...
        nsvgDelete(image_in);
        nsvgDeleteRasterizer(rast);

        std::cerr << std::endl 
                  << "Done for "
                  << fname_in 
                  << "." 
                  << std::endl;
    }
};

/// \brief A class that organizes the processing of SVG assets in PNG files.
///
/// Receives task definition as input and processes them, resulting in PNG 
/// files being written on disk.
///
/// Two main methods are offered: 
///  - parseAndRun(...): Parses a task definition string and immediately
///    processes the request. Blocking call.
///  - parseAndQueue(...): Parses a task definition string and put it at the
///    back of a queue for future processing. Returns immediately. If the 
///    definition is valid it will be processed in the future.
///
/// TODO: Process assets in a thread pool.
/// TODO: Cache the PNG result in memory if the same requests arrives again.
///
class Processor
{
private:
    // The tasks to run queue (FIFO).
    std::queue<TaskDef> task_queue_;

    // The cache hash map (TODO). Note that we use the string definition as the // key.
    using PNGHashMap = std::unordered_map<std::string, PNGDataPtr>;
    PNGHashMap png_cache_;

    std::atomic<bool> should_run_;           // Used to signal the end of the processor to
                                // threads.

    std::vector<std::thread> queue_threads_;

public:
    /// \brief Default constructor.
    ///
    /// Creates background threads that monitors and processes the task queue.
    /// These threads are joined and stopped at the destruction of the instance.
    /// 
    /// \param n_threads: Number of threads (default: NUM_THREADS)
    Processor(int n_threads = NUM_THREADS):
        should_run_(true)
    {
        if (n_threads <= 0) {
            std::cerr << "Warning, incorrect number of threads ("
                      << n_threads
                      << "), setting to "
                      << NUM_THREADS
                      << std::endl;
            n_threads = NUM_THREADS;
        }

        std::cout << "Number of active threads: "<< n_threads << std::endl;

        for (int i = 0; i < n_threads; ++i) {
            queue_threads_.push_back(
                std::thread(&Processor::processQueue, this)
            );
        }
    }

    ~Processor()
    {
        should_run_ = false;
        
        for (auto& qthread: queue_threads_) {
            qthread.join();
        }
    }

    bool fileExistsInSubfolder(const std::string base_name, const fs::path& subfolder) {
        
        std::string stem = fs::path(base_name).stem().string();
        fs::path cache_file = subfolder / "cache.txt";

        std::lock_guard<std::mutex> cache_lock(cache_mutex_);

        // Subfolder must be a valid directory.
        if (!fs::exists(subfolder) || !fs::is_directory(subfolder)) {
            std::cerr << "Subfolder does not exist: " << subfolder << "\n";
            return false;
        }

        // Create the file if it does not exist
        if (!fs::exists(cache_file)) {
            std::ofstream create_file(cache_file);
            if (!create_file) {
                std::cerr << "Failed to create cache file: " << cache_file << "\n";
                return false;
            }
            create_file.close();
        }

        // Read the file line by line to find a match
        std::ifstream infile(cache_file);
        if (!infile) {
            std::cerr << "Failed to open file: " << cache_file << "\n";
            return false;
        }

        std::string line;
        while (std::getline(infile, line)) {
            if (line == stem) {
                std::cout << "Match found for \"" << stem << "\" in cache file.\n";
                return true;
            }
        }
        infile.close();

        // Not found, append fname_in to file
        std::ofstream outfile(cache_file, std::ios::app);
        if (!outfile) {
            std::cerr << "Failed to open file for appending: " << cache_file << "\n";
            return false;
        }

        outfile << stem << '\n';
        std::cout << "Appended \"" << stem << "\" to cache file.\n";
        return false;
    }

    /// \brief Parse a task definition string and fills the references TaskDef
    ///        structure. Returns true if it's a success, false if a failure 
    ///        occured and the structure is not valid.
    bool parse(const std::string& line_org, TaskDef& def)
    {
        std::string line = line_org;
        std::vector<std::string> tokens;
            size_t pos;
            while ((pos = line.find(";")) != std::string::npos) {
                tokens.push_back(line.substr(0, pos));
                line.erase(0, pos + 1);
            }
            tokens.push_back(line);

            if (tokens.size() < 3) {
                std::cerr << "Error: Wrong line format: "
                        << line_org
                        << " (size: " << line_org.size() << ")."
                        << std::endl;
                return false;
            }

            const std::string& fname_in     = tokens[0];
            const std::string& fname_out    = tokens[1];
            const std::string& width_str    = tokens[2]; 

            int width = std::atoi(width_str.c_str());

            def = {
                fname_in,
                fname_out,
                width
            };

            return true;
    }

    /// \brief Tries to parse the given task definition and run it.
    ///
    /// The parsing method will output error messages if it is not valid. 
    /// Nothing occurs if it's the case.
    void parseAndRun(const std::string& line_org)
    {
        TaskDef def;
        if (parse(line_org, def)) {
            TaskRunner runner(def);
            runner();
        }
    }

    /// \brief Parse the task definition and put it in the processing queue.
    ///
    /// If the definition is invalid, error messages are sent to stderr and 
    /// nothing is queued.
    void parseAndQueue(const std::string& line_org)
    {
        std::queue<TaskDef> queue;
        TaskDef def;
        if (parse(line_org, def)) {
            std::cerr << "Queueing task '" << line_org << "'." << std::endl;
            {
                std::lock_guard<std::mutex> lock(queu_mutex);// Lock the queue mutex to ensure thread safety
                task_queue_.push(def);
                queu_updated_signal.release(); // Notify the processing thread that a new task is available
            }
        }
    }

    /// \brief Returns if the internal queue is empty (true) or not.
    bool queueEmpty()
    {
        return task_queue_.empty();
    }

private:
    /// \brief Queue processing thread function.
    void processQueue()
    {
        while (should_run_) {

            queu_updated_signal.try_acquire_for(std::chrono::milliseconds(100)); // Wait for a task to be queued, or timeout after 100ms.

            if (!task_queue_.empty()) {

                take_from_queu_signal.acquire(); // binary semaphore to ensure only one thread takes from the queue at a time
                TaskDef task_def = task_queue_.front();
                task_queue_.pop();
                take_from_queu_signal.release(); // release the semaphore

                fs::path subfolder = "output";
                if ( !fileExistsInSubfolder(task_def.fname_in, subfolder)) { // Check if the file exists in the cache
                    TaskRunner runner(task_def);
                    runner();
                }

            }
        }
    }
};

}

int main(int argc, char** argv)
{
    using namespace gif643;

    std::ifstream file_in;
    int threads = NUM_THREADS;
    if (argc >= 2){
        threads = atoi(argv[1]);
    }
    
    if (argc >= 3 && (strcmp(argv[2], "-") != 0)) {
        file_in.open(argv[2]);
        if (file_in.is_open()) {
            std::cin.rdbuf(file_in.rdbuf());
            std::cerr << "Using " << argv[2] << "..." << std::endl;
        } else {
            std::cerr   << "Error: Cannot open '"
                        << argv[2] 
                        << "', using stdin (press CTRL-D for EOF)." 
                        << std::endl;
        }
    } else {
        std::cerr << "Using stdin (press CTRL-D for EOF)." << std::endl;
    }


    Processor proc(threads);
    
    while (!std::cin.eof()) {

        std::string line, line_org;

        std::getline(std::cin, line);
        if (!line.empty()) {
            proc.parseAndQueue(line);
        }
    }

    if (file_in.is_open()) {
        file_in.close();
    }

    // Wait until the processor queue's has tasks to do.
    while (!proc.queueEmpty()) {};
}
