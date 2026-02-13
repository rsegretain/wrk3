# wrk3

wrk3 is an open-loop HTTP request generator which sends a load to a given service in terms of requests per second.
It is open-loop as the specified rate is maintained regardless of the application's response time or errors.
This is different from a closed-loop generator which waits for requests' responses and thus cannot easily saturate an application. 

Compared to previous version, wrk3 try it's best to send request uniformly across time, not in burst : previous version could send all the required requests for a given second at the same millisecond, then nothing until the next second.

## Main configurations points

### Load profile

It follows instruction from a CSV load file that describe, up to each second, how many requests per seconds it should send.

### Load Lua script

See [SCRIPTING](./SCRIPTING) for more details.

### Duration

The duration of the load injection.
If the load profile describes a shorter load than the set duration, wrk3 will loop the profile.

### Thread number

The number of thread used to generate the load.

### Connections number

The number of connection to keep open between wrk3 and the target

### Connections renewal window

wrk3 can continously renew the connections it use.
This feature exists to work around the default load-balancing strategy of Kubernetes.
By default, Kubernetes distributes the load across the various replicas of a service based on the connections.
If the connections are never renewed, replicas created during the injection never receive their share of the load.

By default, the time window is 20s, it mean that at any time, during the last 20s all connection would have been reset once.

Set the time window to 0 do disable the connections renewal system.

## Usage details
```
Usage: wrk <options> <url>                                       
  Options:                                                       
    -c, --connections <N>  Connections to keep open              
    -D, --dist        <S>  fixed, exp, norm, zipf                
    -P                     Print each request's latency          
    -p                     Print 99th latency every 0.2s to file 
                           under the current working directory   
    -d, --duration    <T>  Duration of test                      
    -t, --threads     <N>  Number of threads to use              
                                                                 
    -s, --script      <S>  Load Lua script file                  
    -H, --header      <H>  Add header to request                 
    -L, --latency          Print latency statistics              
    -S, --separate         Print statistics on different url     
    -T, --timeout     <T>  Socket/request timeout [unit:s]       
    -B, --batch_latency    Measure latency of whole              
                           batches of pipelined ops              
                           (as opposed to each op)               
    -r, --requests         Show the number of sent requests      
    -v, --version          Print version details                 
    -f, --file        <S>  Load rate file                        
    -o, --stats-file <file> Requests stats output file           
    -b, --blocking         Enable blocking mode, connections wait for response
    -w, --window-cx-reset <T>  Duration over whitch all connections are progressively reset [unit:s], default to 20, 0 disable this system
                                                                 
  Numeric arguments may include a SI unit (1k, 1M, 1G)           
  Time arguments may include a time unit (2s, 2m, 2h)
```

## Timeout count limitations

The request in timeout count is very approximative and shouldn't be relied on. Connections are simultaneously used by multiple requests, it's only when a connection doesn't receive a response for more than T seconds that all flying requests on that connection are declared on timeout.
So if a connection receives at least one response often enough, it remains blind to the fact that the other requests can be in timeout.

## Stats file

Output a CSV file containing, for each second, the current count of requests sent, responses received, responses received per HTTP responses code.

Warning : The count of responses can be slightly off, if some responses arrived after wrk3 end.

## Blocking mode

Sometimes it is required to get access to the responses.
When the blocking mode is enabled, wrk3 became close-loop and the connections wait for the response to be received before sending another requests.
Then the response(...) Lua function can correctly match request and response.

# Acknowledgements

## wrk2 DeathStarBench
 
wrk3 isn't directly based on the offical wrk2, it is based on a custom version of wrk2 made by DeathStarBench team :
- [https://doi.org/10.1145/3297858.3304013](https://doi.org/10.1145/3297858.3304013)
- [https://github.com/delimitrou/DeathStarBench/tree/master/wrk2](https://github.com/delimitrou/DeathStarBench/tree/master/wrk2)

## wrk2

[https://github.com/giltene/wrk2](https://github.com/giltene/wrk2)

wrk2 uses Gil Tene's HdrHistogram. Specifically, the C port written
by Mike Barker. Details can be found at http://hdrhistogram.org . Mike
also started the work on this wrk modification, but as he was stuck
on a plane ride to New Zealand, I picked it up and ran it to completion.


## wrk

[https://github.com/wg/wrk](https://github.com/wg/wrk)

wrk contains code from a number of open source projects including the
'ae' event loop from redis, the nginx/joyent/node.js 'http-parser',
Mike Pall's LuaJIT, and the Tiny Mersenne Twister PRNG. Please consult
the NOTICE file for licensing details.

