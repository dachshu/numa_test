#include <iostream>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>
#include <atomic>
#include <algorithm>
#include <iterator>
#include <chrono>
#include <memory>
#include <numa.h>
#include <stack>

using namespace std;

static constexpr int NUM_TEST = 10000000;
static constexpr int RANGE = 1000;

unsigned long fast_rand(void)
{ //period 2^96-1
    static thread_local unsigned long x = 123456789, y = 362436069, z = 521288629;
    unsigned long t;
    x ^= x << 16;
    x ^= x >> 5;
    x ^= x << 1;

    t = x;
    x = y;
    y = z;
    z = t ^ x ^ y;

    return z;
}

struct Node {
public:
	int key;
	Node * volatile next;

	Node() : next{ nullptr } {}
	Node(int key) : key{ key }, next{ nullptr } {}
	~Node() {}
};

bool CAS(Node* volatile * ptr, Node* old_value, Node* new_value) {
	return atomic_compare_exchange_strong(reinterpret_cast<volatile atomic_uintptr_t*>(ptr), reinterpret_cast<uintptr_t*>(&old_value), reinterpret_cast<uintptr_t>(new_value));
}

thread_local unsigned tid;
thread_local unsigned numa_id;

int num_threads;

thread_local int exSize = 1; // thread 별로 교환자 크기를 따로 관리.
constexpr int MAX_PER_THREAD = 32;

//static const unsigned NUM_NUMA_NODES = numa_num_configured_nodes();
//static const unsigned NUM_CPUS = numa_num_configured_cpus();

const unsigned NUM_NUMA_NODES = 4;
const unsigned NUM_CPUS = 64;
const int MAX_THREADS = 128;

class Exchanger {
	volatile int value; // status와 교환값의 합성.

	enum Status { EMPTY, WAIT, BUSY };
	bool CAS(int oldValue, int newValue, Status oldStatus, Status newStatus) {
		int oldV = oldValue << 2 | (int)oldStatus;
		int newV = newValue << 2 | (int)newStatus;
		return atomic_compare_exchange_strong(reinterpret_cast<atomic_int volatile *>(&value), &oldV, newV);
	}

public:
	int exchange(int x) {
		while (true) {
			switch (Status(value & 0x3)) {
			case EMPTY:
			{
				int tempVal = value >> 2;
				if (false == CAS(tempVal, x, EMPTY, WAIT)) continue;

				/* BUSY가 될 때까지 기다리며 timeout된 경우 -1 반환 */
				int count;
				for (count = 0; count < 100; ++count) {
					if (Status(value & 0x3) == BUSY) {
						int ret = value >> 2;
						value = EMPTY;
						return ret;
					}
				}
				if (false == CAS(tempVal, 0, WAIT, EMPTY)) { // 그 사이에 누가 들어온 경우
					int ret = value >> 2;
					value = EMPTY;
					return ret;
				}
				return -1;
			}
			break;
			case WAIT:
			{
				int temp = value >> 2;
				if (false == CAS(temp, x, WAIT, BUSY)) break;
				return temp;
			}
			break;
			case BUSY:
				if (exSize < MAX_PER_THREAD - 1) {
					exSize += 1;
				}
				return x;
			default:
				cerr <<  "It's impossible case\n" ;
				exit(1);
			}
		}
	}

	void init(){
		value = EMPTY;
	}
};

class EliminationArray {
	Exchanger exchanger[MAX_PER_THREAD];

public:
	int visit(int x) {
		int index = fast_rand() % exSize;
		return exchanger[index].exchange(x);
	}

	void shrink() {
		if (exSize > 1) exSize -= 1;
	}

	void init() {
		for(int i = 0; i < MAX_PER_THREAD; ++i){
			exchanger[i].init();
		}
	}
};

enum OP{
	PUSH, POP, EMPTY
};

struct PROPER{
	atomic<OP> op {OP::EMPTY };
	int val { -1 };
};


// Lock-Free Elimination BackOff Stack
class EDLStack {
	stack<int> seq_stack;
	thread helper;
    
    PROPER* propers[MAX_THREADS];

	EliminationArray* eliminationArray[NUM_NUMA_NODES];
public:
	EDLStack()  {
        for(int i = 0; i < NUM_NUMA_NODES; ++i) {
            void *raw_ptr = numa_alloc_onnode(sizeof(EliminationArray), i);
            EliminationArray* ptr = new (raw_ptr) EliminationArray;
            eliminationArray[i] = ptr;
        }

		this->helper = thread{ helper_work };
		unsigned num_core_per_node = NUM_CPUS / NUM_NUMA_NODES;

		for(int i = 0; i < MAX_THREADS; ++i) {
			unsigned alloc_numa_id = (i / num_core_per_node) % NUM_NUMA_NODES;
			void *raw_ptr = numa_alloc_onnode(sizeof(PROPER), alloc_numa_id);
			PROPER* ptr = new (raw_ptr) PROPER;
			propers[i]  = ptr;
		}
		for (auto i = 0; i < MAX_THREADS; ++i)
        {	
			propers[i]->~PROPER();
			numa_free(propers[i], sizeof(PROPER));
        }
    }
    ~EDLStack() {
        for (auto i = 0; i < NUM_NUMA_NODES; ++i)
        {
            eliminationArray[i]->~EliminationArray();
            numa_free(eliminationArray[i], sizeof(EliminationArray));
        }
    }

	void helper_work() {
    	if( -1 == numa_run_on_node(0)){
        	cerr << "Error in pinning thread.. " << endl;
        	exit(1);
    	}
		while (true)
		{
			for(int i = 0 ; i < num_threads; ++i){
				switch (propers[i]->op.load(memory_order_acquire))
				{
				case OP::PUSH:{
					int val = propers[i]->val;
					propers[i]->op.store(OP::EMPTY, memory_order_release);
					seq_stack.push(val);
					break;
				}
				case OP::POP:{
					if (seq_stack.empty()){
						propers[i]->val = 0;
					}
					else{
						propers[i]->val = seq_stack.top();
					}
					
					propers[i]->op.store(OP::EMPTY, memory_order_release);
					seq_stack.pop();

					break;
				}
				default:
					break;
				}
			}
		}
		
	}

	void Push(int x) {
		
		int result = eliminationArray[numa_id]->visit(x);
		if (0 == result) return; // pop과 교환됨.
		if (-1 == result) eliminationArray[numa_id]->shrink(); // timeout 됨.

		propers[tid]->val = x;
		propers[tid]->op.store(OP::PUSH, memory_order_release);
		while (propers[tid]->op.load(memory_order_acquire) != OP::EMPTY) { }
	}

	int Pop() {

		int result = eliminationArray[numa_id]->visit(0);
		//if (0 == result) ; // pop끼리 교환되면 계속 시도
		if (-1 == result) eliminationArray[numa_id]->shrink(); // timeout 됨.
		else return result;

		propers[tid]->op.store(OP::POP, memory_order_release);
		while (propers[tid]->op.load(memory_order_acquire) != OP::EMPTY) { }
		int ret =  propers[tid]->val;
		return ret;
	}

	void clear() {
		for(int i = 0; i < NUM_NUMA_NODES; ++i) {
			eliminationArray[i]->init();
		}
		for (auto i = 0; i < MAX_THREADS; ++i)
        {	
			propers[i]->val = -1;
			propers[i]->op.store(OP::EMPTY);
        }
		while (seq_stack.empty() == false)
		{
			seq_stack.pop();
		}
	}

	void dump(size_t count) {
		cout << count << " Result : ";
		for (auto i = 0; i < count; ++i) {
			if (seq_stack.empty()) break;
			cout << seq_stack.top() << ", ";
		}
		cout << "\n";
	}
} myStack;


void benchMark(int num_thread, int t) {
    tid = t;
    unsigned num_core_per_node = NUM_CPUS / NUM_NUMA_NODES;
    numa_id = (tid / num_core_per_node) % NUM_NUMA_NODES;

    if( -1 == numa_run_on_node(numa_id)){
        cerr << "Error in pinning thread.. " << tid << ", " << numa_id << endl;
        exit(1);
    }
    
	for (int i = 1; i <= NUM_TEST / num_thread; ++i) {
		if ((fast_rand() % 2) || i <= 1000 / num_thread) {
			myStack.Push(i);
		}
		else {
			myStack.Pop();
		}
	}
}

int main() {

	vector<thread> threads;

	for (auto thread_num = 1; thread_num <= 128; thread_num *= 2) {
		myStack.clear();
		threads.clear();
		num_threads = thread_num;

		auto start_t = chrono::high_resolution_clock::now();
        for (int i = 0; i < thread_num; ++i)
            threads.push_back( thread{benchMark, thread_num, i} );
		//generate_n(back_inserter(threads), thread_num, [thread_num]() {return thread{ benchMark, thread_num }; });
		for (auto& t : threads) { t.join(); }
		auto du = chrono::high_resolution_clock::now() - start_t;

		myStack.dump(10);

		cout << thread_num << "Threads, Time = ";
		cout << chrono::duration_cast<chrono::milliseconds>(du).count() << "ms\n";
	}

}