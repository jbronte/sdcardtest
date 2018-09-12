C++ Implmentation of pli-web-server
====================================

This is a status and howto readme for the development of C++98 augment
and/or replacement of pli-web-server. 

Status
======
- C++ parser to process blobs into json is 90% complete
- Subscriber to blobd is sandboxed (C++11) and working, now in development
- All development items are cross-compiled with SDK for Shadow radio and tested on target

Plans
=====
- Finish parser
- Code C++98 subscribe to blobd
- Code udp send to dispatcher
- Test with C++ dispatcher (see dispatcher-lib repo) using different port and output nodes
- Comprehensive design + review

Build Requirements
==================

- Coded for C++98
- Uses rapidjson, not yet added to manifest
- Uses gtest, not added to manifest	   
   
.. code:: sh
   
   $ sudo dnf install rapidjson-devel
   $ sudo dnf install gtest gtest-devel
   
Be sure to set compiler flags to c++98, for example in cmake:   

::

   set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++98 ")

   
   

