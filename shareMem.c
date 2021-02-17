//socket includes
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
//uffd includes
#include <sys/types.h>
#include <stdio.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <poll.h>
#define PORT 5984
#define PORT_TWO 5985
#define BUFF_SIZE 4096

#define errExit(msg) 	do { perror(msg); exit(EXIT_FAILURE); \
 } while (0)

struct MemMsg {
  void * addr;
  int size;
  int page_num;
};

enum msi{ M, S, I }; //enumator for MSI protocol

struct MSImsg {
  int page;
  enum msi state;
};

//array for handling msi cache coherence protocol
static enum msi msi_arr[10];
pthread_mutex_t lock; //mutex for the global msi_arr

static int page_size;

static void *
msi_comm_thread(void *arg)
{
  int sock;
  struct MSImsg msg;
  sock = *(int *) arg;
  for(;;)
  {
    read(sock, &msg, 1024);
    //printf("From comm thread: Received new msi array\n");
 
    pthread_mutex_lock(&lock);
    msi_arr[msg.page] = msg.state;      
    pthread_mutex_unlock(&lock);
    
  }
  
  return NULL; 
}

static void *
fault_handler_thread(void *arg)
{
	static struct uffd_msg msg;   /* Data read from userfaultfd */
	long uffd;                    /* userfaultfd file descriptor */
	static char *page = NULL;
	struct uffdio_copy uffdio_copy;
	ssize_t nread;

	uffd = (long) arg;

	/* [H1: point 1]
	 * Create a mapping of virtual memory for the process at
   * an address chosen by the kernel that has read and write permissions
   * and is visible to other processes.
	 */
	if (page == NULL) {
		page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
		if (page == MAP_FAILED)
			errExit("mmap");
	}

	/* [H2: point 1]
	 * An unconditional for loop, essentially a while(1) loop
	 */
	for (;;) {

		/* See what poll() tells us about the userfaultfd */

		struct pollfd pollfd;
		int nready;

		/* [H3: point 1]
		 * Wait for a page fault event, read the result, then print to console
		 */
		pollfd.fd = uffd;
		pollfd.events = POLLIN;
		nready = poll(&pollfd, 1, -1);
		if (nready == -1)
			errExit("poll");

		/* [H4: point 1]
		 * Read from the user fault page 
		 */
		nread = read(uffd, &msg, sizeof(msg));
		if (nread == 0) {
			printf("EOF on userfaultfd!\n");
			exit(EXIT_FAILURE);
		}

		if (nread == -1)
			errExit("read");

		/* [H5: point 1]
		 * Check for a page fault event and exit if one does not occur
		 */
		if (msg.event != UFFD_EVENT_PAGEFAULT) {
			fprintf(stderr, "Unexpected event on userfaultfd\n");
			exit(EXIT_FAILURE);
		}

		/* [H6: point 1]
		 * Output what kind of event it was and where it happened
		 */
		printf(" [x] PAGE FAULT\n");


		/* [H8: point 1]
		 * fill the uffdio struct using the page parameters set above
		 */
		uffdio_copy.src = (unsigned long) page;
		uffdio_copy.dst = (unsigned long) msg.arg.pagefault.address &
			~(page_size - 1);
		uffdio_copy.len = page_size;
		uffdio_copy.mode = 0;
		uffdio_copy.copy = 0;

		/* [H9: point 1]
		 * Request to continuously copy the the page initialized above into the user fault range
		 */
		if (ioctl(uffd, UFFDIO_COPY, &uffdio_copy) == -1)
			errExit("ioctl-UFFDIO_COPY");
	}
}

int main(int argc, const char *argv[])
{
  
  //Initialize global MSI array
  for(int i = 0; i < 10; i++)
    msi_arr[i] = I;
    
  char *mapping = NULL; //memory map buffer
  int page_num = 0; //number of pages the user wants
  int s; //int for returning error number
  int sock = 0; //int for holding socket fd for msi_array and initial communication
  int sock2 = 0; //sock fd for communicating pages for msi protocol
  page_size = sysconf(_SC_PAGE_SIZE);
  struct MemMsg msg = {NULL, 0, 0}; 
  //userfaultfd params
  long uffd;          /* userfaultfd file descriptor */
 	pthread_t thr;      /* ID of thread that handles page faults */
 	struct uffdio_api uffdio_api;
 	struct uffdio_register uffdio_register;
  
  s = pthread_mutex_init(&lock, NULL);
  if(s != 0)
    errExit("Mutex init failed!");
  
  if(argc < 2) //Instructions
    printf("Usage: ./shareMem <c or s>\n");

  else if(strchr(argv[1], 'c') != NULL) //Client Function
  {
    printf("Used instance as client\n");
   
  	struct sockaddr_in serv_addr, serv_addr2;
  	void *hello = "Successully allocated shared memory!";	
    void *hello2 = "Hello from client 2, ready to send/receive pages!";	
  	
  	//create socket fd
  	if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
  		printf("\n Socket creation error \n");
  		return -1;
  	}
    //create second socket fd
    if ((sock2 = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
  		printf("\n Socket creation error \n");
  		return -1;
  	}
  
  	/* [C2: point 1]
  	 * Explain following in here.
  	 */
  	memset(&serv_addr, '0', sizeof(serv_addr));
  	serv_addr.sin_family = AF_INET;
  	serv_addr.sin_port = htons(PORT);
   
    memset(&serv_addr2, '0', sizeof(serv_addr2));
  	serv_addr2.sin_family = AF_INET;
  	serv_addr2.sin_port = htons(PORT_TWO);
  
  	//set socket parameters
  	if(inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
  		printf("\nInvalid address/ Address not supported \n");
  		return -1;
  	}
    if(inet_pton(AF_INET, "127.0.0.1", &serv_addr2.sin_addr) <= 0) {
  		printf("\nInvalid address/ Address not supported \n");
  		return -1;
  	}
  
  	//connect to server
  	if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
  		printf("\nConnection Failed \n");
  		return -1;
  	}
  
  	printf("Press any key to continue...\n");
  	getchar();
    
    read( sock , &msg, 1024);
    printf("Received Addr: %p, Size: %d \n", msg.addr, msg.size);
    
    //Allocate shared memory here
    mapping = mmap(msg.addr, msg.size, PROT_READ | PROT_WRITE,
  	        	MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    
    page_num = msg.page_num;
    
    //send successfull reply message
    if(mapping != NULL)
    {
 	   send(sock , hello , strlen(hello) , 0 );
  	  printf("Reply message sent\n");
    }
    
    printf("Press any key to continue...\n");
  	getchar();
    
    if (connect(sock2, (struct sockaddr *)&serv_addr2, sizeof(serv_addr2)) < 0) {
  		printf("\nConnection Failed \n");
  		return -1;
  	}
   
    send(sock2, hello2, strlen(hello2), 0);
   
    
  }
  
  else if(strchr(argv[1], 's') != NULL) //Server Function
  {
    printf("Used instance as server\n");
    int server_fd, server_fd2;
  	struct sockaddr_in address, address2;
  	int opt = 1;
    //int page_num = 1;
  	int addrlen = sizeof(address);
    int addrlen2 = sizeof(address2);
  	char buffer[BUFF_SIZE] = {0};
  	//char *hello = "Memory successfully allocated!";   

     
    //Receive number of pages
    printf("> Enter the number of pages you want: \n");
    scanf("%d", &page_num);
    getchar();
  	if(mapping == NULL)
  	{
  		mapping = mmap(NULL, page_num * page_size, PROT_READ | PROT_WRITE,
  	        	MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  	}
    
  
    
  	//create socket fd
  	if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
  		perror("socket failed");
  		exit(EXIT_FAILURE);
  	}
    if ((server_fd2 = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
	  	perror("socket failed");
  	 	exit(EXIT_FAILURE);
 	  }
  
    //set socket options
  	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
  		       &opt, sizeof(opt))) {
  		perror("setsockopt");
  		exit(EXIT_FAILURE);
  	}
    if (setsockopt(server_fd2, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
  		       &opt, sizeof(opt))) {
  		perror("setsockopt");
  		exit(EXIT_FAILURE);
  	}
   
    //set socket params
  	address.sin_family = AF_INET;
  	address.sin_addr.s_addr = INADDR_ANY;
  	address.sin_port = htons( PORT );
    address2.sin_family = AF_INET;
  	address2.sin_addr.s_addr = INADDR_ANY;
  	address2.sin_port = htons( PORT_TWO );
  
  	//bind the socket to the port at the specified address
  	if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
  		perror("bind failed");
  		exit(EXIT_FAILURE);
  	}
    if (bind(server_fd2, (struct sockaddr *)&address2, sizeof(address2)) < 0) {
  		perror("bind failed");
  		exit(EXIT_FAILURE);
  	}
   
  	//listen for clients
  	if (listen(server_fd, 3) < 0) {
  		perror("listen");
  		exit(EXIT_FAILURE);
  	}
  
    //Accept client
  	if ((sock = accept(server_fd, (struct sockaddr *)&address,
  				 (socklen_t*)&addrlen)) < 0) {
  		perror("accept");
  		exit(EXIT_FAILURE);
  	}
   
    msg.addr = mapping;
    msg.size = page_size * page_num;
    msg.page_num = page_num;
    
    printf("Sending Addr: %p, Size: %d\n", msg.addr, msg.size);
  	send(sock, &msg, sizeof(msg) , 0 );
     
    read( sock , buffer, 1024);
    if(buffer != NULL)
  	  printf("Message from client: %s\n",buffer );    
       
    //listen for clients two
    
    printf("Listening for second client\n");
  	if (listen(server_fd2, 3) < 0) {
  		perror("listen");
  		exit(EXIT_FAILURE);
  	}
   
    //Accept client two
  	if ((sock2 = accept(server_fd2, (struct sockaddr *)&address2,
  				 (socklen_t*)&addrlen2)) < 0) {
  		perror("accept");
  		exit(EXIT_FAILURE);
  	}
     
    read( sock2, buffer, 1024);
    printf("Message received from client 2: %s\n", buffer);    
  } 
   //***Starting MSI Protocol*** 
   
   
   //init uffd API
   uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
   if (uffd == -1)
     errExit("userfaultfd");
       
   uffdio_api.api = UFFD_API;
   uffdio_api.features = 0;
   if (ioctl(uffd, UFFDIO_API, &uffdio_api) == -1)
     errExit("ioctl-UFFDIO_API");
   
   uffdio_register.range.start = (unsigned long) mapping;
 	 uffdio_register.range.len = msg.size;
 	 uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING;
 	 if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1)
   	 errExit("ioctl-UFFDIO_REGISTER");
     
   //being pairing memory between machines
   s = pthread_create(&thr, NULL, fault_handler_thread, (void *) uffd);
 	 if (s != 0) {
 		 errno = s;
 		 errExit("pthread_create");
 	 }
   
   
   //thread to receive and update msi array from other application
   pthread_t commThr;
   s = pthread_create(&commThr, NULL, msi_comm_thread, &sock);  
   if (s != 0) {
 		 errno = s;
 		 errExit("pthread_create");
 	 }
    
   int user_page_num;
   char read_write;
   enum msi read_msi = I;
   struct MSImsg mmsg = {0, I};
   //begin read-write command loop
   for(;;){
    
    printf("> Which command should I run? (r:read, w:write, v:view msi list):\n");
    scanf("%c", &read_write);
    getchar();
    
    printf("> For which page? (0-%i, or -1 for all): \n", page_num-1);
    scanf("%d", &user_page_num);
    getchar();
    
    char *start;
    int len = page_size;
    
    //calculate memory params based on user input
    if(user_page_num != -1)
      start = mapping + (page_size * user_page_num);
    else
      start = mapping;
        
    switch(read_write){
    case 'r':
      
      if(user_page_num != -1)
      {
        char buffer[page_size];
        pthread_mutex_lock(&lock); 
        read_msi = msi_arr[user_page_num]; //read msi array
        pthread_mutex_unlock(&lock);   
        
        if(read_msi == I)
        {
          printf("Page %i is Invalid! Requesting page from other application\n", user_page_num); 
          //request page from other application, memcpy to correct region, then change state to shared

          
          pthread_mutex_lock(&lock); 
          msi_arr[user_page_num] = S;
          pthread_mutex_unlock(&lock);
          
          mmsg.page = user_page_num;
          mmsg.state = S;
          send(sock2, &mmsg, sizeof(mmsg), 0);
  
        }  
          
        memcpy(buffer, start, len);
        printf(" [*] Page: %i\n%s\n", user_page_num, buffer);
        
      }
      else
      {
        char buffer[page_size];
        for(int i = 0; i < page_num; i++)
        {
           pthread_mutex_lock(&lock); 
           read_msi = msi_arr[i]; //read msi array
           pthread_mutex_unlock(&lock);   
           if(read_msi == I)
           {
            printf("Page %i is Invalid! Requesting page from other application\n", i);
            pthread_mutex_lock(&lock); 
            msi_arr[i] = S;
            pthread_mutex_unlock(&lock); 
            mmsg.page = i;
            mmsg.state = S;
            send(sock2, &mmsg, sizeof(mmsg), 0);
           }

          memcpy(buffer, start + i*page_size, len);
          printf(" [*] Page: %i\n%s\n", i, buffer);
           
           
        }
          
      }
      break;
    case 'w':
      printf("Chose write option for %d pages\n", user_page_num);
      
      char writeStr[64]; 
      printf("> What would you like to write?\n");
      scanf(" %99[^\n]", writeStr); 
      getchar();
      if(user_page_num != -1)
      {
        memcpy(start, writeStr, strlen(writeStr));
        
        pthread_mutex_lock(&lock); 
        msi_arr[user_page_num] = M;
        pthread_mutex_unlock(&lock); 
        //notify other application that this page is invalid
        mmsg.page = user_page_num;
        mmsg.state = I;
        send(sock2, &mmsg, sizeof(mmsg), 0);
      }
      else
      {
        for(int i = 0; i < page_num; i++)
        {
          memcpy(start+i*page_size, writeStr, strlen(writeStr));
          pthread_mutex_lock(&lock); 
          msi_arr[i] = M;
          pthread_mutex_unlock(&lock); 
          //notify other application that this page is invalid
          mmsg.page = i;
          mmsg.state = I;
          send(sock2, &mmsg, sizeof(mmsg), 0);
        }
      }
      break;
      
    case 'v':
     
      for(int i = 0; i < page_num; i++)
      {
        pthread_mutex_lock(&lock);
        printf("MSI Value for Page %i: %d\n", i, msi_arr[i]);
        pthread_mutex_unlock(&lock);
      } 
      
      break;
      
    default:
      printf("Not a valid option!\n");
      break;
    }
    
    
  } 
	return 0;
}
