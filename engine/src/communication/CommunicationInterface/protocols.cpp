
#include <map>
#include <vector>

#include "protocols.hpp"
#include "messageReceiver.hpp"

#include <ucp/api/ucp.h>
#include <ucp/api/ucp_def.h>
#include <thread>


#include "execution_graph/logic_controllers/CacheMachine.h"

namespace io{
    void read_from_socket(int socket_fd, void * data, size_t read_size){

    }    

    void write_to_socket(int socket_fd, void * data, size_t read_size){

    }    
}


namespace comm {

ucp_nodes_info & ucp_nodes_info::getInstance() {
	static ucp_nodes_info instance;
	return instance;
}

void ucp_nodes_info::init(const std::map<std::string, node> & nodes_map) {
	_id_to_node_info_map = nodes_map;
}

node ucp_nodes_info::get_node(const std::string& id) { return _id_to_node_info_map.at(id); }


graphs_info & graphs_info::getInstance() {
	static graphs_info instance;
	return instance;
}

void graphs_info::register_graph(int32_t ctx_token, std::shared_ptr<ral::cache::graph> graph){
	_ctx_token_to_graph_map.insert({ctx_token, graph});
}

void graphs_info::deregister_graph(int32_t ctx_token){
	std::cout<<"erasing from map"<<std::endl;
	if(_ctx_token_to_graph_map.find(ctx_token) == _ctx_token_to_graph_map.end()){
		std::cout<<"token not found!"<<std::endl;
	}else{
		std::cout<<"erasing token"<<std::endl;
			_ctx_token_to_graph_map.erase(ctx_token);

	}
}
std::shared_ptr<ral::cache::graph> graphs_info::get_graph(int32_t ctx_token) {
	if(_ctx_token_to_graph_map.find(ctx_token) == _ctx_token_to_graph_map.end()){
		std::cout<<"Graph with token"<<ctx_token<<" is not found!"<<std::endl;
		throw std::runtime_error("Graph not found");
	}
	return _ctx_token_to_graph_map.at(ctx_token); }






// TODO: remove this hack when we can modify the ucx_request
// object so we cna send in our c++ callback via the request
std::map<int, ucx_buffer_transport *> message_uid_to_buffer_transport;

std::map<ucp_tag_t, std::shared_ptr<status_code>> recv_begin_ack_status_map;


void send_begin_callback_c(void * request, ucs_status_t status) {
	try
	{
		std::cout<<"send_beging_callback"<<std::endl;
		auto blazing_request = reinterpret_cast<ucx_request *>(request);

		std::cout<<"request "<<request<<std::endl;
		if(message_uid_to_buffer_transport.find(blazing_request->uid) == message_uid_to_buffer_transport.end()){
			std::cout<<"Call back requesting buffer that doesn't exist!!!"<<std::endl;
		}
		auto transport = message_uid_to_buffer_transport[blazing_request->uid];
		transport->recv_begin_transmission_ack();
		ucp_request_release(request);


		// blazing_request->completed = 1;
	}
	catch(const std::exception& e)
	{
		std::cerr << "Error in send_begin_callback_c: " << e.what() << '\n';
	}
}

void recv_begin_ack_callback_c(void * request, ucs_status_t status, ucp_tag_recv_info_t * info) {
	try
	{
		std::cout<<"recieve_beging_ack_callback"<<std::endl;
		std::shared_ptr<status_code> status_begin_ack = recv_begin_ack_status_map[info->sender_tag];

		auto blazing_request = reinterpret_cast<ucx_request *>(request);
		auto transport = message_uid_to_buffer_transport[blazing_request->uid];
		transport->increment_begin_transmission();

		ucp_request_release(request);

		// *status_begin_ack = status_code::OK;
	}
	catch(const std::exception& e)
	{
		std::cerr << "Error in recv_begin_ack_callback_c: " << e.what() << '\n';
	}
}

void send_callback_c(void * request, ucs_status_t status) {
	try{
		std::cout<<"send_callback"<<std::endl;
		auto blazing_request = reinterpret_cast<ucx_request *>(request);
		auto transport = message_uid_to_buffer_transport[blazing_request->uid];
		transport->increment_frame_transmission();
		ucp_request_release(request);
	}
	catch(const std::exception& e)
	{
		std::cerr << "Error in send_callback_c: " << e.what() << '\n';
	}
}


ucx_buffer_transport::ucx_buffer_transport(ucp_worker_h origin_node,
    std::vector<node> destinations,
	ral::cache::MetadataDictionary metadata,
	std::vector<size_t> buffer_sizes,
	std::vector<blazingdb::transport::ColumnTransport> column_transports,
	int ral_id)
	: ral_id{ral_id}, buffer_transport(metadata, buffer_sizes, column_transports,destinations), origin_node(origin_node){
	tag = generate_message_tag();
}

ucx_buffer_transport::~ucx_buffer_transport() {
	message_uid_to_buffer_transport.erase(this->message_id);
}


std::atomic<int> atomic_message_id(0);

ucp_tag_t ucx_buffer_transport::generate_message_tag() {
	std::cout<<"generating tag"<<std::endl;
	auto current_message_id = atomic_message_id.fetch_add(1);
	blazing_ucp_tag blazing_tag = {current_message_id, ral_id, 0u};
	message_uid_to_buffer_transport[blazing_tag.message_id] = this;
	this->message_id = blazing_tag.message_id;
	std::cout<<"almost done"<<std::endl;
	return *reinterpret_cast<ucp_tag_t *>(&blazing_tag);
}

void ucx_buffer_transport::send_begin_transmission() {

	std::cout<<"sending begin transmission"<<std::endl;
	std::vector<char> buffer_to_send = detail::serialize_metadata_and_transports_and_buffer_sizes(metadata, column_transports, buffer_sizes);

	std::vector<char *> requests(destinations.size());
	int i = 0;
	for(auto const & node : destinations) {
		requests[i] = new char[req_size + sizeof(ucx_request) + 1];
		auto temp_tag = *reinterpret_cast<blazing_ucp_tag *>(&tag);
		std::cout<<"sending "<<temp_tag.message_id<<" "<<temp_tag.worker_origin_id <<" "<<temp_tag.frame_id<<std::endl;
		auto status = ucp_tag_send_nbr(
			node.get_ucp_endpoint(), buffer_to_send.data(), buffer_to_send.size(), ucp_dt_make_contig(1), tag, requests[i] + req_size - sizeof(ucx_request));


		if (status == UCS_INPROGRESS) {
			do {
				ucp_worker_progress(origin_node);
    		status = ucp_request_check_status(requests[i] + req_size - sizeof(ucx_request));
			} while (status == UCS_INPROGRESS);
		}
		if(status != UCS_OK){
			std::cout<<"Was not able to send begin transmission" << std::endl;
			throw std::runtime_error("Was not able to send begin transmission to " + node.id());
		}

		std::cout<< "send begin transmition done" << std::endl;

		status_code recv_begin_status = status_code::INVALID;
		ucp_tag_recv_info_t info_tag;
		blazing_ucp_tag acknowledge_tag = *reinterpret_cast<blazing_ucp_tag *>(&tag);
		acknowledge_tag.frame_id = 0xFFFF;


		std::cout<< "recv_begin_transmission_ack recv_nb" << std::endl;
		requests[i] = new char[req_size + sizeof(ucx_request) + 1];
		std::cout<<"listening for "<<acknowledge_tag.message_id<<" "<<acknowledge_tag.worker_origin_id <<" "<<acknowledge_tag.frame_id<<std::endl;

		// ucp_tag_recv_info_t	info;
		// auto stat =	ucp_tag_probe_nb(origin_node,
		// 														*reinterpret_cast<ucp_tag_t *>(&acknowledge_tag),
		// 														acknownledge_tag_mask,
		// 														1,
		// 														&info);
		// while(!stat){
		// 	//ucp_worker_progress(origin_nodes);
		// 	stat =	ucp_tag_probe_nb(origin_node,
		// 													*reinterpret_cast<ucp_tag_t *>(&acknowledge_tag),
		// 													acknownledge_tag_mask,
		// 													1,
		// 													&info);
		// }

		// temp_tag = *reinterpret_cast<blazing_ucp_tag *>(info.sender_tag);
		// std::cout<<"probed tag is  "<<temp_tag.message_id<<" "<<temp_tag.worker_origin_id <<" "<<temp_tag.frame_id<<std::endl;
		// std::cout<<"message length was "<<info.length<<std::endl;
		status = ucp_tag_recv_nbr(origin_node,
									&recv_begin_status,
									sizeof(status_code),
									ucp_dt_make_contig(1),
									*reinterpret_cast<ucp_tag_t *>(&acknowledge_tag),
									acknownledge_tag_mask,
									requests[i] + req_size - sizeof(ucx_request));

		if (status == UCS_INPROGRESS) {
			do {
				ucp_worker_progress(origin_node);
				ucp_tag_recv_info_t info_tag;
    		status = ucp_tag_recv_request_test(requests[i] + req_size - sizeof(ucx_request), &info_tag);
			} while (status == UCS_INPROGRESS);
		}
		if(status != UCS_OK){
			std::cout<<"Was not able to receive acknowledgment of begin transmission" << std::endl;
			throw std::runtime_error("Was not able to receive acknowledgment of begin transmission from " + node.id());
		}

		std::cout<<"ack status == status_code::OK = "<<(recv_begin_status == status_code::OK)<<std::endl;
		if (recv_begin_status == status_code::OK) {
			increment_begin_transmission();
		}

		// this->recv_begin_transmission_ack();

		i++;
	}
}


void ucx_buffer_transport::wait_until_complete() {
	std::unique_lock<std::mutex> lock(mutex);
	completion_condition_variable.wait(lock, [this] {
		if(transmitted_frames >= (buffer_sizes.size() * destinations.size())) {
			return true;
		} else {
			return false;
		}
	});
	std::cout<< "FINISHED WAITING wait_until_complete"<<std::endl;
}

void ucx_buffer_transport::recv_begin_transmission_ack() {
	std::cout<<"going to receive ack!!"<<std::endl;
	auto recv_begin_status = std::make_shared<status_code>(status_code::INVALID);
	std::shared_ptr<ucp_tag_recv_info_t> info_tag = std::make_shared<ucp_tag_recv_info_t>();
	blazing_ucp_tag acknowledge_tag = *reinterpret_cast<blazing_ucp_tag *>(&tag);
	acknowledge_tag.frame_id = 0xFFFF;

	ucp_worker_h ucp_worker = origin_node;
	ucp_tag_message_h message_tag;

	for(;;) {
		// std::cout<<"inside loop for probe!!"<<std::endl;
		message_tag = ucp_tag_probe_nb(ucp_worker,
			*reinterpret_cast<ucp_tag_t *>(&acknowledge_tag),
			acknownledge_tag_mask, 1, info_tag.get());

		if (message_tag != NULL) {
				/* Message arrived */
				break;
		}

		// else if (ucp_worker_progress(ucp_worker)) {
		// 		/* Some events were polled; try again without going to sleep */
		// 		continue;
		// }
	}

	std::cout<< "recv_begin_transmission_ack recv_nb" << std::endl;
	recv_begin_ack_status_map[tag] = recv_begin_status;
	auto request = ucp_tag_msg_recv_nb(ucp_worker,
																		recv_begin_status.get(),
																		info_tag->length,
																		ucp_dt_make_contig(1),
																		message_tag,
																		recv_begin_ack_callback_c);

	if(UCS_PTR_IS_ERR(request)) {
		// TODO: decide how to do cleanup i think we just throw an initialization exception
	} else if(UCS_PTR_STATUS(request) == UCS_OK){
		increment_begin_transmission();
	}	else {
		// assert(UCS_PTR_IS_PTR(request));
    // while (*recv_begin_status != status_code::OK) {
    //     ucp_worker_progress(ucp_worker);
    // }
		// auto transport = message_uid_to_buffer_transport[blazing_request->uid];
		// transport->increment_begin_transmission();

		// ucp_request_release(request);

		auto blazing_request = reinterpret_cast<ucx_request *>(request);
		blazing_request->uid = reinterpret_cast<blazing_ucp_tag *>(&tag)->message_id;

	}
}

void ucx_buffer_transport::send_impl(const char * buffer, size_t buffer_size) {
	std::vector<ucs_status_ptr_t> requests;
	blazing_ucp_tag blazing_tag = *reinterpret_cast<blazing_ucp_tag *>(&tag);
	blazing_tag.frame_id = buffer_sent + 1;	 // 0th position is for the begin_message
	for(auto const & node : destinations) {
		requests.push_back(ucp_tag_send_nb(node.get_ucp_endpoint(),
			buffer,
			buffer_size,
			ucp_dt_make_contig(1),
			*reinterpret_cast<ucp_tag_t *>(&blazing_tag),
			send_callback_c));
	}

	for(auto & request : requests) {
		if(UCS_PTR_IS_ERR(request)) {
			// TODO: decide how to do cleanup i think we just throw an initialization exception
		} else if(UCS_PTR_STATUS(request) == UCS_OK) {
			increment_frame_transmission();
		} else {
			// Message was not completed we set the uid for the callback
			auto blazing_request = reinterpret_cast<ucx_request *>(&request);
			blazing_request->uid = reinterpret_cast<blazing_ucp_tag *>(&tag)->message_id;
		}
	}
	// TODO: call ucp_worker_progress here
}


std::map<void *,std::shared_ptr<status_code> > status_scope_holder;

void send_acknowledge_callback_c(void * request, ucs_status_t status){
	try{
		std::cout<<"send ack callback"<<std::endl;
		status_scope_holder.erase(request);
		ucp_request_release(request);
	}
	catch(const std::exception& e)
	{
		std::cerr << "Error in send_acknowledge_callback_c: " << e.what() << '\n';
	}
}




static void flush_callback(void *request, ucs_status_t status)
{
}


/*
tcp_buffer_transport::tcp_buffer_transport(
        std::vector<node> destinations,
		ral::cache::MetadataDictionary metadata,
		std::vector<size_t> buffer_sizes,
		std::vector<blazingdb::transport::ColumnTransport> column_transports,
        int ral_id,
        ctpl::thread_pool<BlazingThread> * allocate_copy_buffer_pool) 
        : ral_id{ral_id}, buffer_transport(metadata, buffer_sizes, column_transports,destinations),
	    allocate_copy_buffer_pool{allocate_copy_buffer_pool} {

        //Initialize connection to get

    for(auto destination : destinations){
        int socket_fd; 
        struct sockaddr_in address; 

        if ((socket_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) 
        { 
            throw std::runtime_error("Could not open communication socket");
        } 
        address.sin_family = AF_INET; 
        address.sin_port = htons(destination.port()); 

        if(inet_pton(AF_INET, destination.ip().c_str(), &address.sin_addr)<=0)  
        { 
            throw std::runtime_error("Invalid Communication Address");
        } 
        if (connect(socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0) 
        { 
            throw std::runtime_error("Invalid Communication Address");
        } 
        socket_fds.push_back(socket_fd);
    }
}

void tcp_buffer_transport::send_begin_transmission(){
    status_code status;
    std::vector<char> buffer_to_send = detail::serialize_metadata_and_transports_and_buffer_sizes(metadata, column_transports,buffer_sizes);
	auto size_to_send = buffer_to_send.size();

    for (auto socket_fd : socket_fds){       
        //write out begin_message_size
        io::write_to_socket(socket_fd, &size_to_send ,sizeof(size_to_send));
        io::write_to_socket(socket_fd, buffer_to_send.data(),buffer_to_send.size());
        io::read_from_socket(socket_fd,&status, sizeof(status_code));
        if(status != status_code::OK){
            throw std::runtime_error("Could not send begin transmission");
        }
        increment_begin_transmission();
    }

}

void tcp_buffer_transport::send_impl(const char * buffer, size_t buffer_size){
    //this is where it gets fuzzy...

    //allocate pinned + copy from gpu
    //transmit
    size_t pinned_buffer_size = getPinnedBufferProvider().sizeBuffers();
    size_t num_chunks = (buffer_size +(pinned_buffer_size - 1))/ pinned_buffer_size;
    
    std::vector<std::future<PinnedBuffer *> > buffers;
    for( size_t chunk = 0; chunk < num_chunks; chunk++ ){
        size_t chunk_size = pinned_buffer_size;
        auto buffer_chunk = buffer + (chunk * pinned_buffer_size);
        if(( chunk + 1) == num_chunks){
            chunk_size = buffer_size - (chunk * pinned_buffer_size);
        }
        buffers.append(
            std::move(allocate_copy_buffer_pool.push(
                [buffer_chunk,chunk_size](int thread_id) {
                    auto pinned_buffer = getPinnedBufferProvider().getBuffer();
                    pinned_buffer->use_size = 
                    cudaMemcpyAsync(pinned_buffer->data,buffer_chunk,chunk_size,cuaMemcpyDeviceToHost,pinned_buffer->stream);                    
                    return pinned_buffer;
            }))
        );
    }
    size_t chunk = 0;
    try{
        while(chunk < num_chunks){
            auto pinned_buffer = buffers[chunk].get();
            cudaStreamSynchronize(pinned_buffer->stream);
            for (auto socket_fd : socket_fds){
                io::write_to_socket(socket_fd, &pinned_buffer->use_size,sizeof(pinned_buffer->use_size));
                io::write_to_socket(socket_fd, pinned_buffer->data,pinned_buffer->use_size);
                increment_frame_transmission();
            }
            getPinnedBufferProvider().freeBuffer(pinned_buffer);
        }
    }catch(const std::exception & e ){
        throw;
    }

}

tcp_buffer_transport::~tcp_buffer_transport(){
    for (auto socket_fd : socket_fds){
        close(socket_fd);
    }
}


*/

}  // namespace comm
