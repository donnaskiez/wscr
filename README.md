# Brief

I saw some tweet about an interview question where you make a
multithreaded web crawler. Sounded interesting as I have no
understanding of what a web crawler actually is and haven't done
much web programming. As a bonus challenge I want to try and minimise
my use of AI. I used some extension [1] on Chrome to disable Gemini
search results in chrome.

- Pass in a URL as argument
- Parse the URLs in the page
- Don't crawl the same link twice
- Only explore links that are under the same hostname as the URL arg

[1] https://chromewebstore.google.com/detail/hide-gemini-and-google-ai/ggneoaeoplbpehkojnifancipajnhcbp

```
# Design

## Connection

The first thing that was needed is some way to download a web page.
For whatever reason I ended up using WinINet. Only later did I find
out about WinHTTP which is the modern way for doing this as
WinINet is meant for GUI applications and those things. Additionally
WinHTTP is asynchronous and can be used with completion ports,
but at this point I don't feel like rewriting and thats OK since
we learnt a bit about esoteric Internet Explorer APIs.

We first want to setup some context for the session which requires
a session handle and a connection handle. The session handle is
created when we first initialise WinINet. The more important handle
is our connection handle. Since the task specifies we are only
processings child URLs of the target hostname, we only need to
create a single handle when we first establish the connection
to the seed URL, and for subsequent calls we can reuse this
existing handle as this handle is used for all calls to a specified
hostname [1].

### Connection Options

As opposed to what the docs say, HTTP2 is not enabled by default.
HTTP2 enables us to multiple requests with a single connection,
which as we increase thread count will increase speed signicantly.
Alongside this we will want to set our maximum connection count
to 64. Both of is done via the InternetSetOptionA API prior
to opening the session handle.

## Url Parsing

Url parsing is mostly by InternetCrackUrlA which turns a null
terminated string and breaks it into sections. We can specify
which sections of the URL we want (i.e for example we could
specify just the hostname).

This will be important as we only care about URLs that match our
hostname aswell as other URL normalisation such as ensuring we
handle case-insentitive scheme and hostnames.

We also need to combine relative Urls and append them to the parents
URL as they are not relative to the seed hostname (unlike robots.txt)
but relative to the parents URL.

## Download

Using this connection handle we can use the Http* family of APIs.

- HttpOpenRequestA to open a handle to some opaque request object
- HttpSendRequestA to send the request
- HttpQueryInfoA to query parts of the request reponse such as
  the status code and page length.
- InternetReadFile to finally read the page in chunks as specified
  by the documentation [2].

These are all blocking APIs, so the vast majority of the runtime is
actually downloading these pages. Using WinHTTP in theory may help
but the same problem still applies. You still need to download the
page prior to being able to parse it, and the same applies to all
child URLs in the page. The more important part of this challenge
is ensuring we are sending the maximum number of HTTP requests
at all times.

### Parsing Page

Once we have the page in a buffer, we simply enumerate the page
looking for URLs. We want to make sure we don't count URLs in
a comment block. Once we've found a URL, we break it down into
components and compare hostnames (among other things) against
our seed. We then ensure the page is allowed to be crawled
as specified by robots.txt

#### Robots.txt

robots.txt is a file that lives in the root of the server and
determines which part of the website can be parsed (among
other things that I am not interested in like sitmaps). A sample
may look like:

User-agent: *
Disallow: /*//
Disallow: //
Disallow: *:
Disallow: /speed_test
Disallow: /infinite
Disallow: /robots_protocol/robots_excluded_1
Disallow: /robots_protocol/robots_excluded_2
Disallow: /robots_protocol/robots_excluded_3

Where `User-agent` specifies the name of the user agent that must
follow these set of rules. In our case we only care about the `*`
user agent (i.e all) and of course the we don't care about the
Allow rules, only the disallow rules.

When we first setup the connection to the seed, the first thing
we do is download and parse the robots.txt file. If one does
not exist, we proceed and allow everything. If there is one,
we find all rules associated with our user agent (i.e *) and
store them in a list. This list will be enumerated everytime
we find a new URL to ensure the URl we parse is allowed to
be crawled. Those that aren't allowed by the rules are
discarded.

The pattern matching is fairly straight forward, except for
the '*' token. Additionally rules are written relative
to the hostname and so the path component is all we care about
when parsing. Googles open source robots.txt parse [3] was an
excellent source for this implementation, it is also very simple
and doesn't handle many other tokens.

#### URL Duplication

Since the task states we must not visit duplicate URLs, we
need some way to keep track of URLs we have visited. The obvious
solution is a simple hashset. We use the basic FNV hash
function and implement the psuedocode found in [4]. Each
bucket then has a list that we enumerate. We compare first
via the absolute URL hash (which is the entire URL, including
hostname), then length and then finally memory comparison.
Theres not much more to add here, it is very basic with
a simple AddOrContains primitive that we call prior to
inserting a page into our queue.

This more or less completes the task in a single threaded manner
but it obviously is quite slow. To improve the speed we need to
split the work up across threads.

## Threading

### Allocator

Prior to this, we use a bare bones arena allocator that uses TLS
to store the arena context. This is important as it will determine
how we setup our threads. I followed [5] for writing the allocator
and a few random videos to learn about it.

### Threadpool Implementation

Win32 offers a complete set of threadpool APIs [6], but utilising these
APIs mean you lose access to your custom allocated TLS storage, which
means we need to synchronise our allocator and this is not something
I wanted to do.

Instead we've implemented our own basic threadpool. The basic idea is
an array of handles is created that point to thread objects (it's
crucial this is an array such that we can use WaitForMultiple) and
we start the threads suspended. It does make the error case slightly
more complex, but I like to do this such that we can use ResumeThread
to start the threads and split initialisation from start.

Then we simply use WaitForMultiplObjects on the handle array to
wait for our threads to complete prior to cleanup.

#### Thread Count

Even though I only have 20 logical cores, we want to ensure
we match our maximum concurrent connections to the server. In this case
it is 64. This means we can have 64 total concurrent requests.
This does significantly increase our memory footprint,
especially since I commit immediately and don't reserve and
incrementally commit, but thats Okay :).

### Queue

When we have a new unique URL, we insert it into a simple
list based queue. Since we are now multithreaded, we need someway
to ensure safety when accessing the queue. Additionally we need to
alert threads when work is ready, especially since we'll be waiting
on network traffic quite a bit. The easy solution in usermode is a
slim RW lock and a condition variable. Each time we access the queue
we acquire the SRW lock, and if the queue is empty we atomically
release the SRW lock and sleep on the condition variable via
SleepConditionVariableSRW which handles this race condition for us.
When the condition variable is alerted and this thread wins the
condition variable, the SRW lock is guaranteed to be held when our
thread continues execution, meaning we can pop a page and release
the lock.

When pushing an item into the queue, make sure we use
WakeConditionVariable which wakes a single waiter on our condition
variable, and not all. The synchronisation is fairly simple
and much of the complexity is actually handled by Windows.

### Terminating

A tricky part is knowing when to terminate, and for that we have
a very simple reference counter. When a page is inserted, we want
to increment our refcount by 1, this means that only when all child
pages of the parent page (and all children fo those children etc.)
have completed (as the first page is the seed) is it safe to terminate.
i.e when this refcount finally reaches 0.

Every time we finish processing a page, we check this count and if
it is 0 we indicate via the termination flag and wake all condition
variables to get the threads to exit. The WaitForMultiple
that is waiting on the handle array will then yield once all threads
have been exited and the program will complete.

## Conclusion

Overall it was a fun project, and it was good to go back to the days
of having 50 win32 API documentation tabs open lol. I also learnt quite
alot about HTTP, URLs and arena allocators among other things. The
scraper ended up being pretty good, and a sample run of [7] is:

[18:18:50.348] [22888] ==================================
[18:18:50.348] [22888] Pages Parsed: 1194
[18:18:50.348] [22888] Pages Robots Blocked: 0
[18:18:50.348] [22888] Time Taken: 18.69s
[18:18:50.348] [22888] ==================================

# References

[1] https://learn.microsoft.com/en-us/windows/win32/api/wininet/nf-wininet-internetconnecta
[2] https://learn.microsoft.com/en-us/windows/win32/api/wininet/nf-wininet-internetreadfile
[3] https://github.com/google/robotstxt/blob/master/robots.cc
[4] https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function
[5] https://andreleite.com/posts/2025/nstl/virtual-memory-arena-allocator/
[6] https://learn.microsoft.com/en-us/windows/win32/procthread/thread-pool-api
[7] https://books.toscrape.com/
```