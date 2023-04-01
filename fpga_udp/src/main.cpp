#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include <iostream>
#include <thread>
#include <future>
#include <mutex>
#include <cmath>
#ifdef _WIN32
    #include <Winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "Ws2_32.lib")
#elif __linux__
    #include <sys/socket.h>
    #include <netinet/in.h>
#endif
#include "WzSerialportPlus.h"
#include "mmw_example_nonos.h"

#define STRINGIFY(x) #x
#define MACRO_STRINGIFY(x) STRINGIFY(x)

namespace py = pybind11;

WzSerialportPlus radar_serial_port;
int memidx=0;
int overflowCnt=0;
int packetSize_g=1456;
uint32_t packetNum_g=1000;
uint8_t *serial_buf_ptr=NULL;
std::mutex serial_buf_mutex;
std::mutex udp_mutex;
std::mutex udp_async_mutex;
std::future<pybind11::array_t<uint8_t>> udp_val;
uint32_t receivedPacketNum_g=0,firstPacketNum_g=0,lastPacketNum_g=0,expectedPacketNum_g=0;
// py::array_t<uint8_t> doubleBuffer[2];
// bool firstBufFull=false;

int add(int i, int j) {
    return i+j;
}

bool radar_start_read_thread(std::string portName, int baudrate, uint32_t packetNum, int packetSize){
    if(serial_buf_ptr!=NULL)
        return false;
    serial_buf_ptr = new uint8_t[packetSize*packetNum];//initialize buffer
    packetNum_g=packetNum;
    packetSize_g=packetSize;

    radar_serial_port.setReceiveCalback([&](char* data,int length){//Set the receive callback function and execute it after receiving the data
        serial_buf_mutex.lock();//exclusive buffer
        if(memidx+length>packetSize_g*packetNum_g){//Guaranteed not to exceed the buffer range
            overflowCnt++;
            printf("[fpga_udp]serial buffer overflowed for %d time(s)!memidx=%d,length=%d,packetSize=%d,packetNum=%d\n",overflowCnt,memidx,length,packetSize_g,packetNum_g);
            memidx=0;
        }
        memcpy(serial_buf_ptr+memidx,data,length);//Copy the received data into the buffer
        memidx+=length;
        serial_buf_mutex.unlock();
    });

    #ifdef _WIN32
        if (portName.substr(0,3)=="COM" && stoi(portName.substr(3))>=10)
            portName="\\\\.\\"+portName;//port name has prefix in windows
    #endif
    return radar_serial_port.open(portName,baudrate,1,8,'n');//Open the port to start the thread to receive data
}

py::array_t<uint8_t> get_radar_buf(){
    serial_buf_mutex.lock();//exclusive buffer
    auto result = py::array_t<uint8_t>(memidx);
    py::buffer_info buf = result.request();
    uint8_t *buf_ptr = static_cast<uint8_t *>(buf.ptr);
    memcpy(buf_ptr,serial_buf_ptr,memidx);
    memidx=0;
    serial_buf_mutex.unlock();
    return result;
}

void radar_stop_read_thread(){
    radar_serial_port.close();
    serial_buf_mutex.lock();
    delete[] serial_buf_ptr;
    serial_buf_ptr=NULL;
    memidx=0;
    overflowCnt=0;
    serial_buf_mutex.unlock();
}

void _read_data_udp(int sock_fd, uint32_t frameNum, int bytesInFrame, int packetSize, int &lastFrameRemainBytes, int timeout_s, py::array_t<uint8_t> buf){
    udp_mutex.lock();

    if(packetSize%2 != 0){
        throw std::runtime_error("[fpga_udp]Number of packetSize must be even");
    }

    py::buffer_info buf_info = buf.request();
    uint8_t *buf_ptr = static_cast<uint8_t *>(buf_info.ptr);

    // Time out
    #ifdef __linux__
        int sockfd = sock_fd;
        struct timeval tv;
        tv.tv_sec  = timeout_s;
        tv.tv_usec = 0;
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(struct timeval));
    #elif _WIN32
        SOCKET sockfd = (SOCKET)sock_fd;
        int TimeOut = timeout_s*1000;
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&TimeOut, sizeof(TimeOut));
    #endif

    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);
    memset(&src, 0, sizeof(src));

    uint32_t seqNum=0;
    uint64_t bytesCnt;
    int receivePacketLen=0,lastFrameTransferedBytes;

    // Wait for start of next frame
    do{
        receivePacketLen=recvfrom(sockfd, (char*)&buf_ptr[0], packetSize, 0, (sockaddr*)&src, &src_len);
        if (receivePacketLen < 0){      //blocking receive timeout
            printf("[fpga_udp]udp time out\n");
            udp_mutex.unlock();
            lastFrameRemainBytes=-1;
            return;
        }
        seqNum = *(uint32_t *)buf_ptr;//first 4 bytes: sequence number for every packet
        bytesCnt = (seqNum-1)*(packetSize-10);
        lastFrameTransferedBytes = bytesCnt % bytesInFrame;
        lastFrameRemainBytes = (bytesInFrame-lastFrameTransferedBytes) % bytesInFrame;
    }while(lastFrameRemainBytes>=packetSize-10);
    
    expectedPacketNum_g = ceil((lastFrameRemainBytes + frameNum*bytesInFrame)/((double)(packetSize-10)));

    // Read in the rest of the frame
    for(int i=1;i<expectedPacketNum_g;i++){
        int idx = i*packetSize;
        // if(idx>=packetSize*packetNum){
        //     throw std::runtime_error("Array Index Out Of Bounds!");
        // }
        receivePacketLen=recvfrom(sockfd, (char*)&buf_ptr[idx], packetSize, 0, (sockaddr*)&src, &src_len);
        if (receivePacketLen < 0){      //blocking receive timeout
            printf("[fpga_udp]udp time out\n");
            break;
        }
    }

    udp_mutex.unlock();
}

py::array_t<uint8_t> postProc_packet_sort(py::array_t<uint8_t> buf, uint32_t frameNum, int bytesInFrame, int packetSize, int lastFrameRemainBytes, uint32_t &receivedPacketNum, uint32_t &firstPacketNum, uint32_t &lastPacketNum){
    int payloadSize = packetSize-10, cpySize;
    uint32_t packetNum = ceil((lastFrameRemainBytes + frameNum*bytesInFrame)/((double)(packetSize-10)));
    uint32_t seqNum=0,idx=0;

    auto result = py::array_t<uint8_t>(frameNum*bytesInFrame);
    py::buffer_info sort_buf_info = result.request();
    uint8_t *sort_buf_ptr = static_cast<uint8_t *>(sort_buf_info.ptr);

    py::buffer_info buf_info = buf.request();
    uint8_t *buf_ptr = static_cast<uint8_t *>(buf_info.ptr);

    //first packet
    firstPacketNum = *(uint32_t *)buf_ptr;//4 bytes of sequence number for every packet
    cpySize = payloadSize-lastFrameRemainBytes;
    memcpy(sort_buf_ptr, &buf_ptr[lastFrameRemainBytes], cpySize);
    receivedPacketNum = 1;

    //rest packet
    for(uint32_t i=1;i<packetNum;i++){
        seqNum=*(uint32_t *)&buf_ptr[i*packetSize];//4 bytes of sequence number for every packet
        //byteCnt=*(uint32_t *)&buf_ptr[i*packetSize+4];//6 bytes of byte count – provides information about number of bytes transmitted till last packet.
        idx = seqNum-firstPacketNum;
        if(idx<1 || idx>packetNum-1) continue;//out of boundary
        if(idx==packetNum-1){//last packet
            memcpy(&sort_buf_ptr[(idx-1)*payloadSize+cpySize], &buf_ptr[i*packetSize+10], frameNum*bytesInFrame-idx*payloadSize+lastFrameRemainBytes);
        }else{//middle packet
            memcpy(&sort_buf_ptr[(idx-1)*payloadSize+cpySize], &buf_ptr[i*packetSize+10], payloadSize);//1456 bytes of payload
        }
        receivedPacketNum++;
    }

    lastPacketNum = *(uint32_t *)&buf_ptr[(packetNum-1)*packetSize];
    // printf("[fpga_udp]received packet num:%d,expected packet num:%d\n",receivedPacketNum,packetNum);

    return result;
}

// void _read_data_udp_buffered(int sock_fd, uint32_t bufFrameNum, int bytesInFrame, int packetSize, int timeout_s){
//     uint32_t maxPacketNum = ((bufFrameNum+1)*bytesInFrame)/(packetSize-10)+1;
//     firstBufFull=false;
//     auto packetBuf = py::array_t<uint8_t>(maxPacketNum*packetSize);
//     doubleBuffer[0] = py::array_t<uint8_t>(bufFrameNum*bytesInFrame);
//     doubleBuffer[1] = py::array_t<uint8_t>(bufFrameNum*bytesInFrame);
// }

py::array_t<uint8_t> read_data_udp(int sock_fd, uint32_t frameNum, int bytesInFrame, int packetSize, int timeout_s, bool sort){
    uint32_t maxPacketNum = ((frameNum+1)*bytesInFrame)/(packetSize-10)+1;
    /* No pointer is passed, so NumPy will allocate the buffer */
    auto result = py::array_t<uint8_t>(maxPacketNum*packetSize);
    int lastFrameRemainBytes=0;

    _read_data_udp(sock_fd,frameNum,bytesInFrame,packetSize,lastFrameRemainBytes,timeout_s,result);

    if(sort) return postProc_packet_sort(result,frameNum,bytesInFrame,packetSize,lastFrameRemainBytes,receivedPacketNum_g,firstPacketNum_g,lastPacketNum_g);

    return result;
}

py::array_t<uint8_t> read_data_udp_block_thread(int sock_fd, uint32_t frameNum, int bytesInFrame, int packetSize, int timeout_s, bool sort){
    uint32_t maxPacketNum = ((frameNum+1)*bytesInFrame)/(packetSize-10)+1;
    /* No pointer is passed, so NumPy will allocate the buffer */
    auto result = py::array_t<uint8_t>(maxPacketNum*packetSize);
    int lastFrameRemainBytes=0;

    std::thread udp_thread(_read_data_udp,sock_fd,frameNum,bytesInFrame,packetSize,std::ref(lastFrameRemainBytes),timeout_s,result);
    udp_thread.join();

    if(sort) return postProc_packet_sort(result,frameNum,bytesInFrame,packetSize,lastFrameRemainBytes,receivedPacketNum_g,firstPacketNum_g,lastPacketNum_g);

    return result;
}

void read_data_udp_async_start(int sock_fd, uint32_t frameNum, int bytesInFrame, int packetSize, int timeout_s, bool sort){
    udp_async_mutex.lock();
    udp_val = std::async(std::launch::async, read_data_udp, sock_fd,frameNum,bytesInFrame,packetSize,timeout_s,sort);
}

py::array_t<uint8_t> read_data_udp_async_wait(){
    udp_async_mutex.unlock();
    return udp_val.get();
}

PYBIND11_MODULE(fpga_udp, m) {
    m.doc() = R"pbdoc(
        FPGA UDP reader & mmwavelink API plugin
        -----------------------

        .. currentmodule:: fpga_udp

        .. autosummary::
           :toctree: _generate

           add
           subtract

           radar_start_read_thread
           get_radar_buf
           radar_stop_read_thread
           getOverflowCnt

           read_data_udp
           read_data_udp_block_thread
           read_data_udp_async_start
           read_data_udp_async_wait
           get_receivedPacketNum
           get_firstPacketNum
           get_lastPacketNum
           get_expectedPacketNum

           AWR2243_firmwareDownload
           AWR2243_init
           AWR2243_setFrameCfg
           AWR2243_sensorStart
           AWR2243_waitSensorStop
           AWR2243_sensorStop
           AWR2243_sensorStartCont
           AWR2243_sensorStopCont
           AWR2243_poweroff
           AWR2243_test_demo_app
    )pbdoc";

    m.def("radar_start_read_thread", &radar_start_read_thread);
    m.def("get_radar_buf", &get_radar_buf);
    m.def("radar_stop_read_thread", &radar_stop_read_thread);
    m.def("getOverflowCnt", []() {
        return overflowCnt;
    });
    m.def("reset_radar_buf", []() {
        serial_buf_mutex.lock();
        memidx=0;
        overflowCnt=0;
        serial_buf_mutex.unlock();
    });

    m.def("read_data_udp", &read_data_udp, R"pbdoc(
        read UDP packet from FPGA as fast as written in C.

        sockfd:\tan open file descriptor of a socket
        frameNum:\tA total of frameNum frames will be read
        bytesInFrame:\tbytes of one frame
        packetSize:\tand the size of each packet is packetSize
        timeout_s:\ttime out for udp in sec
    )pbdoc");
    m.def("read_data_udp_block_thread", &read_data_udp_block_thread, R"pbdoc(
        read UDP packet from FPGA using thread, block untill data transfer finished.

        sockfd:\tan open file descriptor of a socket
        frameNum:\tA total of frameNum frames will be read
        bytesInFrame:\tbytes of one frame
        packetSize:\tand the size of each packet is packetSize
        timeout_s:\ttime out for udp in sec
    )pbdoc");
    m.def("read_data_udp_async_start", &read_data_udp_async_start, R"pbdoc(
        read UDP packet from FPGA using async, return immediately.

        sockfd:\tan open file descriptor of a socket
        frameNum:\tA total of frameNum frames will be read
        bytesInFrame:\tbytes of one frame
        packetSize:\tand the size of each packet is packetSize
        timeout_s:\ttime out for udp in sec
    )pbdoc");
    m.def("read_data_udp_async_wait", &read_data_udp_async_wait, R"pbdoc(
        read UDP packet from FPGA using async, wait previous async task finished.
    )pbdoc");
    m.def("get_receivedPacketNum", []() {return receivedPacketNum_g;});
    m.def("get_firstPacketNum", []() {return firstPacketNum_g;});
    m.def("get_lastPacketNum", []() {return lastPacketNum_g;});
    m.def("get_expectedPacketNum", []() {return expectedPacketNum_g;});

    m.def("AWR2243_firmwareDownload",[](){
        return MMWL_App_firmwareDownload(RL_DEVICE_MAP_CASCADED_1);
    }, R"pbdoc(
        This function downloads meta image patch to external serial flash.
        Once the flashing operation is successful, 
        it is not necessary to download the meta image in the subsequent power cycles.
        If no flash connected or not downloaded successfully, you must set downloadFw=true
        when you call AWR2243_init and set EnableFwDownload=1 in mmwaveconfig.txt and
        MMWL_FILETYPE_META_IMG = 0 in mmw_example_nonos.h.
    )pbdoc");
    m.def("AWR2243_init",[](std::string configFilename){
        return MMWL_App_init(RL_DEVICE_MAP_CASCADED_1, configFilename.c_str(), false);
    });
    m.def("AWR2243_setFrameCfg",[](int numFrames){
        return MMWL_App_setFrameCfg(RL_DEVICE_MAP_CASCADED_1, numFrames);
    });
    m.def("AWR2243_sensorStart",[](){
        return MMWL_App_startSensor(RL_DEVICE_MAP_CASCADED_1);
    });
    m.def("AWR2243_waitSensorStop",[](){
        return MMWL_App_waitSensorStop(RL_DEVICE_MAP_CASCADED_1);
    });
    m.def("AWR2243_sensorStop",[](){
        return MMWL_App_stopSensor(RL_DEVICE_MAP_CASCADED_1);
    });
    m.def("AWR2243_sensorStartCont",[](){
        return MMWL_App_startCont(RL_DEVICE_MAP_CASCADED_1);
    }, R"pbdoc(
        This function starts the FMCW radar in continous mode.
        In continuous mode, the signal is not frequency modulated but has the same frequency over time.
        Note: The continuous streaming mode configuration APIs are supported only for debug purpose.
        Please refer latest DFP release note for more info.
    )pbdoc");
    m.def("AWR2243_sensorStopCont",[](){
        return MMWL_App_stopCont(RL_DEVICE_MAP_CASCADED_1);
    });
    m.def("AWR2243_poweroff",[](){
        return MMWL_App_poweroff(RL_DEVICE_MAP_CASCADED_1);
    });
    m.def("AWR2243_test_demo_app",[](std::string configFilename){
        return MMWL_App(configFilename.c_str());
    });
    
    m.def("add", &add, R"pbdoc(
        Add two numbers
    )pbdoc");

    m.def("subtract", [](int i, int j) {
        return i - j;
    });

#ifdef VERSION_INFO
    m.attr("__version__") = MACRO_STRINGIFY(VERSION_INFO);
#else
    m.attr("__version__") = "dev";
#endif
}
