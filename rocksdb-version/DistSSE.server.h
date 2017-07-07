/*
 * Created by Xiangfu Song on 10/21/2016.
 * Email: bintasong@gmail.com
 * 
 */
#ifndef DISTSSE_SERVER_H
#define DISTSSE_SERVER_H

#include <grpc++/grpc++.h>

#include "DistSSE.grpc.pb.h"

#include "DistSSE.Util.h"

#include "logger.h"

#define min(x ,y) ( x < y ? x : y)

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerWriter;
using grpc::Status;

namespace DistSSE{

class DistSSEServiceImpl final : public RPC::Service {
private:	
	static rocksdb::DB* ss_db;
	rocksdb::DB* cache_db;
    int MAX_THREADS;

public:
	DistSSEServiceImpl(const std::string db_path, const std::string cache_path, int concurrent){
		rocksdb::Options options;

		    rocksdb::CuckooTableOptions cuckoo_options;
            cuckoo_options.identity_as_first_hash = false;
            cuckoo_options.hash_table_ratio = 0.9;
            
            
            cuckoo_options.use_module_hash = false;
            cuckoo_options.identity_as_first_hash = true;
            

            options.table_cache_numshardbits = 4;
            options.max_open_files = -1;
            
			options.allow_concurrent_memtable_write = true;
    		options.enable_write_thread_adaptive_yield = true;
            
			options.table_factory.reset(rocksdb::NewCuckooTableFactory(cuckoo_options));
            
            // options.memtable_factory.reset(new rocksdb::VectorRepFactory());
            
            options.compression = rocksdb::kNoCompression;
            options.bottommost_compression = rocksdb::kDisableCompressionOption;
            
            options.compaction_style = rocksdb::kCompactionStyleLevel;
            options.info_log_level = rocksdb::InfoLogLevel::INFO_LEVEL;
            
            
            // options.max_grandparent_overlap_factor = 10;
            
            options.delayed_write_rate = 8388608;
            options.max_background_compactions = 20;
            
            options.disableDataSync = true;
            options.allow_mmap_reads = true;
            options.new_table_reader_for_compaction_inputs = true;
            
            options.max_bytes_for_level_base = 4294967296;
            options.arena_block_size = 134217728;
            options.level0_file_num_compaction_trigger = 10;
            options.level0_slowdown_writes_trigger = 16;
            options.hard_pending_compaction_bytes_limit = 137438953472;
            options.target_file_size_base=201327616;
            options.write_buffer_size=1073741824;
    		options.create_if_missing = true;

    	rocksdb::Status s1 = rocksdb::DB::Open(options, db_path, &ss_db);
    	rocksdb::Status s2 = rocksdb::DB::Open(options, cache_path, &cache_db);
			
			if (!s1.ok() || !s2.ok()) {
                logger::log(logger::CRITICAL) << "Unable to open the database: " << s1.ToString() <<", s2:"<< s2.ToString()<< std::endl;
                // db_ = NULL;
            }
		MAX_THREADS = concurrent; //std::thread::hardware_concurrency();
	}

	static int store(rocksdb::DB* &db, const std::string l, const std::string e){
		rocksdb::Status s = db->Put(rocksdb::WriteOptions(), l, e);
		if (s.ok())	return 0;
		else return -1;
	}

	static std::string get(rocksdb::DB* &db, const std::string l){
		std::string tmp;
		rocksdb::Status s = db->Get(rocksdb::ReadOptions(), l, &tmp);
		if (s.ok())	return tmp;
		else return "";
	}


	static void parse (std::string str, std::string& op, std::string& ind) {
		op = str.substr(0, 1);		
		ind = str.substr(1, 7); // TODO
	}

	static void search_task(int threadID, std::string kw, int begin, int end, std::set<std::string>* result_set) {
		std::string ind, op;
		std::string l, e, value;

		for(int i = begin + 1; i <= end; i++) {
			l = Util::H1(kw + std::to_string(i));
			// logger::log(logger::INFO) << "server.search(), l:" << l << ", kw: " << kw <<std::endl;
			e = get(ss_db, l);
			value = Util::Xor( e, Util::H2(kw + std::to_string(i)) );

			parse(value, op, ind);

			// logger::log(logger::INFO) << "value: " << value <<std::endl;
			/*if(op == "1")*/  result_set->insert(value); // TODO
			// else result_set->erase(ind);
			if (i % 1000 == 0) logger::log(logger::INFO) << "Thread ID: " << threadID << ", searched: " << i << "\n" <<std::flush;
		}
	}

	static void merge( std::set<std::string>* searchResult, int counter, std::set<std::string> &mergeResult){
		for (int i = 0; i < counter; i++) {
			for (auto& t : searchResult[i]) {
				mergeResult.insert(t);
			}
		}
	}

	void search(std::string kw, std::string tw, int uc, std::set<std::string>& ID){
	
		std::vector<std::string> op_ind;

		std::string ind, op;
		std::string l, e, value;
		std::string cache_ind;

		int counter = 0;

		struct timeval t1, t2;		

		gettimeofday(&t1, NULL);

		cache_ind = get(cache_db, tw);
		Util::split(cache_ind, '|', ID); // get all cached inds

		if(kw == "") {
		gettimeofday(&t2, NULL);

		logger::log(logger::INFO) <<"[ <==ONLY CACHE==> ] "<<"ID.size():"<< ID.size() <<" ,search time: "<< ((t2.tv_sec - t1.tv_sec) * 1000000.0 + t2.tv_usec -t1.tv_usec) /1000.0/ID.size()<<" ms" <<std::endl;

			return; // if kw == "", no need to search on ss_db.
		}

		//================ God bless =================
		std::set<std::string>* result_set = new std::set<std::string>[MAX_THREADS]; // result ID lists for storage nodes

		std::vector<std::thread> threads;
		int step = uc / MAX_THREADS;

        for (int i = 0; i < MAX_THREADS; i++) {
        	threads.push_back( std::thread(search_task, i,  kw, i * step, min((i + 1) * step, uc), &(result_set[i])) );
		}
		// join theads
    	for (auto& t : threads) {
        	t.join();
    	}

		merge(result_set, MAX_THREADS, ID);	
		
/*
		for(int i = 1; i <= uc; i++) {
			l = Util::H1(kw + std::to_string(i));
			// logger::log(logger::INFO) << "server.search(), l:" << l << ", kw: " << kw <<std::endl;
			e = get(ss_db, l);
			value = Util::Xor( e, Util::H2(kw + std::to_string(i)) );
			// std::vector<std::string> op_ind;
			ID.insert(value);
			Util::split(value, '|', op_ind);
			op = op_ind[0];
			ind = op_ind[1];
			if(op == "1") result_set->insert(ind); // TODO
			else result_set->erase(ind);
		}
*/

		gettimeofday(&t2, NULL);

  		logger::log(logger::INFO) <<"ID.size():"<< ID.size() <<" ,search time: "<< ((t2.tv_sec - t1.tv_sec) * 1000000.0 + t2.tv_usec - t1.tv_usec) /1000.0/ID.size()<<" ms" <<std::endl;

		//==============================================

		std::string ID_string = "";
		for (std::set<std::string>::iterator it=ID.begin(); it!=ID.end(); ++it){
    		ID_string += Util::str2hex(*it) + "|";
		}
		store(cache_db, tw, ID_string);
	}

// server RPC
	// search() 实现搜索操作
	Status search(ServerContext* context, const SearchRequestMessage* request,
                  ServerWriter<SearchReply>* writer)  {

		std::string kw = request->kw();
		std::string tw = request->tw();	
		int uc = request->uc();
		
		struct timeval t1, t2;

		// TODO 读取数据库之前要加锁，读取之后要解锁
		
		std::set<std::string> ID;

		logger::log(logger::INFO) << "searching... " <<std::endl;

		// gettimeofday(&t1, NULL);
		search(kw, tw, uc, ID);
		// gettimeofday(&t2, NULL);

  		// logger::log(logger::INFO) <<"ID.size():"<< ID.size() <<" ,search time: "<< ((t2.tv_sec - t1.tv_sec) * 1000000.0 + t2.tv_usec - t1.tv_usec) /1000.0/ID.size()<<" ms" <<std::endl;
		// TODO 读取之后需要解锁

		SearchReply reply;
		
		for(int i = 0; i < ID.size(); i++){
			reply.set_ind(std::to_string(i));
			writer->Write(reply);
		}

		logger::log(logger::INFO) << "search done." <<std::endl;

	    return Status::OK;
  	}
	
	// update()实现单次更新操作
	Status update(ServerContext* context, const UpdateRequestMessage* request, ExecuteStatus* response) {
		std::string l = request->l();
		std::string e = request->e();
		//std::cout<<"ut: "<<ut<< " # " <<"enc_value: "<<enc_value<<std::endl;
		// TODO 更新数据库之前要加锁
		//logger::log(logger::INFO) <<".";
		int status = store(ss_db, l, e);
		// TODO 更新之后需要解锁
		//logger::log(logger::INFO) << "*" << std::endl;
		logger::log(logger::INFO) << "UPDATE fuck" << std::endl;
		if(status != 0) {
			response->set_status(false);
			return Status::CANCELLED;
		}
		response->set_status(true);
		return Status::OK;
	}
	
	// batch_update()实现批量更新操作
	Status batch_update(ServerContext* context, ServerReader< UpdateRequestMessage >* reader, ExecuteStatus* response) {
		std::string l;
		std::string e;
		// TODO 读取数据库之前要加锁，读取之后要解锁
		UpdateRequestMessage request;
		while (reader->Read(&request)){
			l = request.l();
			e = request.e();
			store(ss_db, l, e);
		}
		// TODO 读取之后需要解锁

		response->set_status(true);
		return Status::OK;
	}
};

}// namespace DistSSE

// static member must declare out of main function !!!
rocksdb::DB* DistSSE::DistSSEServiceImpl::ss_db;

void RunServer(std::string db_path, std::string cache_path, int concurrent) {
  std::string server_address("0.0.0.0:50051");
  DistSSE::DistSSEServiceImpl service(db_path, cache_path, concurrent);

  ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());

  builder.RegisterService(&service);

  std::unique_ptr<Server> server(builder.BuildAndStart());
  DistSSE::logger::log(DistSSE::logger::INFO) << "Server listening on " << server_address << std::endl;

  server->Wait();
}

#endif // DISTSSE_SERVER_H