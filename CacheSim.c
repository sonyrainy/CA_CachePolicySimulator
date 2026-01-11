
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <float.h>

// Cache sizes: 1024, 2048, 4096, 8192, 16384 bytes
// Block sizes: 8, 16, 32, 64, 128 bytes
// Associativity: 1 (direct), 2, 4, 8

#define NUM_CACHE 5
#define NUM_BLOCK 5
#define NUM_ASSOC 4

// Output tables 관련 내용
#define NUM_ROWS (NUM_ASSOC * 2)
#define NUM_COLS (NUM_BLOCK * NUM_CACHE)

static const int CACHE_SIZES[NUM_CACHE] = { 1024, 2048, 4096, 8192, 16384 };
static const int BLOCK_SIZES[NUM_BLOCK] = { 8, 16, 32, 64, 128 };
static const int ASSOC_LIST[NUM_ASSOC] = { 1, 2, 4, 8 };

#define MAX_ASSOC 8

// LRU 
struct Block_LRU {
	// Block_* 하나가 "블록 1개"가 아니라
	// 실제로는 "세트 1개"를 의미한다.
	// 아래 배열들이 그 세트 안의 여러 way(라인)들을 나타낸다.
	unsigned long tag[MAX_ASSOC];
	unsigned char valid[MAX_ASSOC];
	unsigned char write_back[MAX_ASSOC];
};
static struct Block_LRU* icah_lru = NULL;
static struct Block_LRU* dcah_lru = NULL;

// FIFO
struct Block_FIFO {
	unsigned long tag[MAX_ASSOC];
	unsigned char valid[MAX_ASSOC];
	unsigned char write_back[MAX_ASSOC];
};
static struct Block_FIFO* icah_fifo = NULL;
static struct Block_FIFO* dcah_fifo = NULL;
static int* icah_fifo_ptr = NULL;
static int* dcah_fifo_ptr = NULL;

// NEW (Frequency Based Counter Policy)
struct Block_NEW {
	unsigned long tag[MAX_ASSOC];
	unsigned char valid[MAX_ASSOC];
	unsigned char write_back[MAX_ASSOC];

	// 0(신규/교체대상) ~ 3(자주 사용/보존대상)
	unsigned char priority_counter[MAX_ASSOC];
};
static struct Block_NEW* icah_new = NULL;
static struct Block_NEW* dcah_new = NULL;
static int* icah_new_ptr = NULL;
static int* dcah_new_ptr = NULL;


static inline unsigned long get_block_addr(unsigned long addr, int block_size) {
	return addr / (unsigned long)block_size;
}
static inline int get_index(unsigned long block_addr, int num_sets) {
	return (int)(block_addr % (unsigned long)num_sets);
}
static inline unsigned long get_tag(unsigned long block_addr, int num_sets) {
	return block_addr / (unsigned long)num_sets;
}
static inline int col_idx(int block_idx, int cache_idx) {
	return block_idx * NUM_CACHE + cache_idx;
}
static inline int row_i(int assoc_idx) { return assoc_idx; }
static inline int row_d(int assoc_idx) { return NUM_ASSOC + assoc_idx; }

static void die_oom(void) {
	fprintf(stderr, "Out of memory.\n");
	exit(1);
}


static void lru_move_to_front(struct Block_LRU* set, int pos, int assoc) {
	if (pos <= 0) return;
	unsigned long t = set->tag[pos];
	unsigned char v = set->valid[pos];
	unsigned char d = set->write_back[pos];
	for (int i = pos; i > 0; i--) {
		set->tag[i] = set->tag[i - 1];
		set->valid[i] = set->valid[i - 1];
		set->write_back[i] = set->write_back[i - 1];
	}
	set->tag[0] = t;
	set->valid[0] = v;
	set->write_back[0] = d;
}

static int access_lru(struct Block_LRU* cache, int num_sets, int assoc, int block_size,
	unsigned long addr, int is_write,
	long* pmiss, int* pwritebacks) {

	unsigned long baddr = get_block_addr(addr, block_size);
	int index = get_index(baddr, num_sets);
	unsigned long tag = get_tag(baddr, num_sets);

	struct Block_LRU* set = &cache[index];

	int hit_pos = -1;
	for (int w = 0; w < assoc; w++) {
		if (set->valid[w] && set->tag[w] == tag) {
			hit_pos = w;
			break;
		}
	}

	if (hit_pos >= 0) {
		if (is_write) set->write_back[hit_pos] = 1;
		lru_move_to_front(set, hit_pos, assoc);
		return 1;
	}

	(*pmiss)++;

	int victim = -1;
	for (int w = assoc - 1; w >= 0; w--) {
		if (!set->valid[w]) {
			victim = w;
			break;
		}
	}
	if (victim < 0) victim = assoc - 1;

	if (set->valid[victim] && set->write_back[victim]) {
		(*pwritebacks)++;
	}

	set->tag[victim] = tag;
	set->valid[victim] = 1;
	set->write_back[victim] = (unsigned char)(is_write ? 1 : 0);
	lru_move_to_front(set, victim, assoc);
	return 0;
}


static int access_fifo(struct Block_FIFO* cache, int* ptr, int num_sets, int assoc, int block_size,
	unsigned long addr, int is_write,
	long* pmiss, int* pwritebacks) {

	unsigned long baddr = get_block_addr(addr, block_size);
	int index = get_index(baddr, num_sets);
	unsigned long tag = get_tag(baddr, num_sets);

	struct Block_FIFO* set = &cache[index];

	for (int w = 0; w < assoc; w++) {
		if (set->valid[w] && set->tag[w] == tag) {
			if (is_write) set->write_back[w] = 1;
			return 1;
		}
	}

	(*pmiss)++;
	int victim = ptr[index];

	if (set->valid[victim] && set->write_back[victim]) {
		(*pwritebacks)++;
	}

	set->tag[victim] = tag;
	set->valid[victim] = 1;
	set->write_back[victim] = (unsigned char)(is_write ? 1 : 0);

	ptr[index] = (ptr[index] + 1) % assoc;
	return 0;
}


static int access_new(struct Block_NEW* cache, int* ptr, int num_sets, int assoc, int block_size,
	unsigned long addr, int is_write,
	long* pmiss, int* pwritebacks) {

	unsigned long baddr = get_block_addr(addr, block_size);
	int index = get_index(baddr, num_sets);
	unsigned long tag = get_tag(baddr, num_sets);

	struct Block_NEW* set = &cache[index];

	// HIT 체크
	for (int w = 0; w < assoc; w++) {
		if (set->valid[w] && set->tag[w] == tag) {

			// Hit 되면 점수를 올림 (최대 3점)
			if (set->priority_counter[w] < 3) {
				set->priority_counter[w]++;
			}

			if (is_write) set->write_back[w] = 1;
			return 1;
		}
	}

	// MISS Handling
	(*pmiss)++;

	int victim = -1;

	// Victim 찾기: 점수가 0인 것을 찾을 때까지 반복
	while (victim == -1) {
		for (int w = 0; w < assoc; w++) {
			// 빈 공간이 있으면 1순위
			if (!set->valid[w]) {
				victim = w;
				break;
			}
			if (set->priority_counter[w] == 0) {
				victim = w;
				break;
			}
		}

		if (victim != -1) break;

		for (int w = 0; w < assoc; w++) {
			if (set->priority_counter[w] > 0) set->priority_counter[w]--;
		}
	}

	if (set->valid[victim] && set->write_back[victim]) {
		(*pwritebacks)++;
	}

	set->tag[victim] = tag;
	set->valid[victim] = 1;
	set->write_back[victim] = (unsigned char)(is_write ? 1 : 0);

	set->priority_counter[victim] = 1;

	(void)ptr;
	return 0;
}


static void simulate_lru(int* type, unsigned long* addr, int length,
	double miss[NUM_ROWS][NUM_COLS],
	int writes[NUM_ROWS][NUM_COLS],
	int i_totals[NUM_ROWS][NUM_COLS],
	int d_totals[NUM_ROWS][NUM_COLS]);

static void simulate_fifo(int* type, unsigned long* addr, int length,
	double miss[NUM_ROWS][NUM_COLS],
	int writes[NUM_ROWS][NUM_COLS],
	int i_totals[NUM_ROWS][NUM_COLS],
	int d_totals[NUM_ROWS][NUM_COLS]);

static void simulate_new(int* type, unsigned long* addr, int length,
	double miss[NUM_ROWS][NUM_COLS],
	int writes[NUM_ROWS][NUM_COLS],
	int i_totals[NUM_ROWS][NUM_COLS],
	int d_totals[NUM_ROWS][NUM_COLS]);

static void print_results(const char* label,
	const double miss[NUM_ROWS][NUM_COLS],
	const int writes[NUM_ROWS][NUM_COLS]);

static void print_best_results(
	const double lru_miss[NUM_ROWS][NUM_COLS], const int lru_writes[NUM_ROWS][NUM_COLS],
	const int lru_i_tot[NUM_ROWS][NUM_COLS], const int lru_d_tot[NUM_ROWS][NUM_COLS],
	const double fifo_miss[NUM_ROWS][NUM_COLS], const int fifo_writes[NUM_ROWS][NUM_COLS],
	const int fifo_i_tot[NUM_ROWS][NUM_COLS], const int fifo_d_tot[NUM_ROWS][NUM_COLS],
	int i_hit, int i_miss, int d_hit, int d_miss);

static void read_trace(const char* path,
	int** ptype, unsigned long** paddr, int* plen);

static void print_two_cache_state(const char* policy,
	int is_icache, int index, int assoc);

static void print_new_cache_state(int is_icache, int index, int assoc);

static void usage(const char* prog) {
	fprintf(stderr,
		"Usage: %s <policy> <trace_file> [cycle_params]\n"
		"  <policy>        FIFO, LRU, NEW or BEST (case-insensitive)\n"
		"  <trace_file>    input trace in .txt format\n"
		"  [cycle_params]  Required only for BEST policy:\n"
		"                    <i_hit> <i_miss> <d_hit> <d_miss>\n"
		"  Example (FIFO):  %s FIFO trace1.txt\n"
		"  Example (LRU):   %s LRU trace1.txt\n"
		"  Example (NEW):   %s NEW trace1.txt\n"
		"  Example (BEST):  %s BEST trace1.txt 1 100 1 50\n",
		prog, prog, prog, prog, prog);
	exit(1);
}

static void read_trace(const char* path, int** ptype, unsigned long** paddr, int* plen) {

	FILE* fp = fopen(path, "r");
	if (!fp) {
		fprintf(stderr, "Failed to open trace file: %s\n", path);
		exit(1);
	}

	int cap = 1 << 20;
	int len = 0;

	int* types = (int*)malloc(sizeof(int) * cap);
	unsigned long* addrs = (unsigned long*)malloc(sizeof(unsigned long) * cap);
	if (!types || !addrs) die_oom();

	int ts = 0, label = 0;
	unsigned long addr = 0;

	while (fscanf(fp, "%d %d %lx", &ts, &label, &addr) == 3) {
		(void)ts;
		if (len >= cap) {
			cap *= 2;
			int* ntypes = (int*)realloc(types, sizeof(int) * cap);
			unsigned long* naddrs = (unsigned long*)realloc(addrs, sizeof(unsigned long) * cap);
			if (!ntypes || !naddrs) die_oom();
			types = ntypes;
			addrs = naddrs;
		}
		types[len] = label;
		addrs[len] = addr;
		len++;
	}

	fclose(fp);

	*ptype = types;
	*paddr = addrs;
	*plen = len;
}

static void simulate_lru(int* type, unsigned long* addr, int length,
	double miss[NUM_ROWS][NUM_COLS],
	int writes[NUM_ROWS][NUM_COLS],
	int i_totals[NUM_ROWS][NUM_COLS],
	int d_totals[NUM_ROWS][NUM_COLS]) {

	for (int a = 0; a < NUM_ASSOC; a++) {
		int assoc = ASSOC_LIST[a];
		int r_i = row_i(a);
		int r_d = row_d(a);

		for (int b = 0; b < NUM_BLOCK; b++) {
			int block = BLOCK_SIZES[b];

			for (int c = 0; c < NUM_CACHE; c++) {
				int cache_size = CACHE_SIZES[c];
				int col = col_idx(b, c);

				int num_sets = cache_size / (block * assoc);

				icah_lru = (struct Block_LRU*)calloc((size_t)num_sets, sizeof(struct Block_LRU));
				dcah_lru = (struct Block_LRU*)calloc((size_t)num_sets, sizeof(struct Block_LRU));
				if (!icah_lru || !dcah_lru) die_oom();

				long i_acc = 0, i_miss = 0;
				long d_acc = 0, d_miss = 0;
				int d_writebacks = 0;

				for (int t = 0; t < length; t++) {
					if (type[t] == 2) {
						i_acc++;
						(void)access_lru(icah_lru, num_sets, assoc, block, addr[t], 0, &i_miss, &(int){0});
					}
					else if (type[t] == 0) {
						d_acc++;
						(void)access_lru(dcah_lru, num_sets, assoc, block, addr[t], 0, &d_miss, &d_writebacks);
					}
					else if (type[t] == 1) {
						d_acc++;
						(void)access_lru(dcah_lru, num_sets, assoc, block, addr[t], 1, &d_miss, &d_writebacks);
					}
				}

				miss[r_i][col] = (i_acc == 0) ? 0.0 : ((double)i_miss / (double)i_acc);
				miss[r_d][col] = (d_acc == 0) ? 0.0 : ((double)d_miss / (double)d_acc);

				writes[r_i][col] = 0;
				writes[r_d][col] = d_writebacks;

				i_totals[r_i][col] = (int)i_acc;
				d_totals[r_d][col] = (int)d_acc;

				free(icah_lru);
				free(dcah_lru);
				icah_lru = NULL;
				dcah_lru = NULL;
			}
		}
	}
}

static void simulate_fifo(int* type, unsigned long* addr, int length,
	double miss[NUM_ROWS][NUM_COLS],
	int writes[NUM_ROWS][NUM_COLS],
	int i_totals[NUM_ROWS][NUM_COLS],
	int d_totals[NUM_ROWS][NUM_COLS]) {

	for (int a = 0; a < NUM_ASSOC; a++) {
		int assoc = ASSOC_LIST[a];
		int r_i = row_i(a);
		int r_d = row_d(a);

		for (int b = 0; b < NUM_BLOCK; b++) {
			int block = BLOCK_SIZES[b];

			for (int c = 0; c < NUM_CACHE; c++) {
				int cache_size = CACHE_SIZES[c];
				int col = col_idx(b, c);

				int num_sets = cache_size / (block * assoc);

				icah_fifo = (struct Block_FIFO*)calloc((size_t)num_sets, sizeof(struct Block_FIFO));
				dcah_fifo = (struct Block_FIFO*)calloc((size_t)num_sets, sizeof(struct Block_FIFO));
				icah_fifo_ptr = (int*)calloc((size_t)num_sets, sizeof(int));
				dcah_fifo_ptr = (int*)calloc((size_t)num_sets, sizeof(int));
				if (!icah_fifo || !dcah_fifo || !icah_fifo_ptr || !dcah_fifo_ptr) die_oom();

				long i_acc = 0, i_miss = 0;
				long d_acc = 0, d_miss = 0;
				int d_writebacks = 0;

				for (int t = 0; t < length; t++) {
					if (type[t] == 2) {
						i_acc++;
						(void)access_fifo(icah_fifo, icah_fifo_ptr, num_sets, assoc, block, addr[t], 0, &i_miss, &(int){0});
					}
					else if (type[t] == 0) {
						d_acc++;
						(void)access_fifo(dcah_fifo, dcah_fifo_ptr, num_sets, assoc, block, addr[t], 0, &d_miss, &d_writebacks);
					}
					else if (type[t] == 1) {
						d_acc++;
						(void)access_fifo(dcah_fifo, dcah_fifo_ptr, num_sets, assoc, block, addr[t], 1, &d_miss, &d_writebacks);
					}
				}

				miss[r_i][col] = (i_acc == 0) ? 0.0 : ((double)i_miss / (double)i_acc);
				miss[r_d][col] = (d_acc == 0) ? 0.0 : ((double)d_miss / (double)d_acc);

				writes[r_i][col] = 0;
				writes[r_d][col] = d_writebacks;

				i_totals[r_i][col] = (int)i_acc;
				d_totals[r_d][col] = (int)d_acc;

				free(icah_fifo);
				free(dcah_fifo);
				free(icah_fifo_ptr);
				free(dcah_fifo_ptr);
				icah_fifo = NULL;
				dcah_fifo = NULL;
				icah_fifo_ptr = NULL;
				dcah_fifo_ptr = NULL;
			}
		}
	}
}

static void simulate_new(int* type, unsigned long* addr, int length,
	double miss[NUM_ROWS][NUM_COLS],
	int writes[NUM_ROWS][NUM_COLS],
	int i_totals[NUM_ROWS][NUM_COLS],
	int d_totals[NUM_ROWS][NUM_COLS]) {

	for (int a = 0; a < NUM_ASSOC; a++) {
		int assoc = ASSOC_LIST[a];
		int r_i = row_i(a);
		int r_d = row_d(a);

		for (int b = 0; b < NUM_BLOCK; b++) {
			int block = BLOCK_SIZES[b];

			for (int c = 0; c < NUM_CACHE; c++) {
				int cache_size = CACHE_SIZES[c];
				int col = col_idx(b, c);

				int num_sets = cache_size / (block * assoc);

				icah_new = (struct Block_NEW*)calloc((size_t)num_sets, sizeof(struct Block_NEW));
				dcah_new = (struct Block_NEW*)calloc((size_t)num_sets, sizeof(struct Block_NEW));
				icah_new_ptr = (int*)calloc((size_t)num_sets, sizeof(int));
				dcah_new_ptr = (int*)calloc((size_t)num_sets, sizeof(int));
				if (!icah_new || !dcah_new || !icah_new_ptr || !dcah_new_ptr) die_oom();

				long i_acc = 0, i_miss = 0;
				long d_acc = 0, d_miss = 0;
				int d_writebacks = 0;

				for (int t = 0; t < length; t++) {

					//if (0 && t == 20) {
					if (t == 20) {

						print_new_cache_state(1, 0, assoc);
						print_new_cache_state(0, 0, assoc);
					}

					if (type[t] == 2) {
						i_acc++;
						(void)access_new(icah_new, icah_new_ptr, num_sets, assoc, block,
							addr[t], 0, &i_miss, &(int){0});
					}
					else if (type[t] == 0) {
						d_acc++;
						(void)access_new(dcah_new, dcah_new_ptr, num_sets, assoc, block,
							addr[t], 0, &d_miss, &d_writebacks);
					}
					else if (type[t] == 1) {
						d_acc++;
						(void)access_new(dcah_new, dcah_new_ptr, num_sets, assoc, block,
							addr[t], 1, &d_miss, &d_writebacks);
					}
				}

				miss[r_i][col] = (i_acc == 0) ? 0.0 : ((double)i_miss / (double)i_acc);
				miss[r_d][col] = (d_acc == 0) ? 0.0 : ((double)d_miss / (double)d_acc);

				writes[r_i][col] = 0;
				writes[r_d][col] = d_writebacks;

				i_totals[r_i][col] = (int)i_acc;
				d_totals[r_d][col] = (int)d_acc;

				free(icah_new);
				free(dcah_new);
				free(icah_new_ptr);
				free(dcah_new_ptr);
				icah_new = NULL;
				dcah_new = NULL;
				icah_new_ptr = NULL;
				dcah_new_ptr = NULL;
			}
		}
	}
}

static void print_results(const char* label,
	const double miss[NUM_ROWS][NUM_COLS],
	const int writes[NUM_ROWS][NUM_COLS]) {
	int i, j, k;

	printf("\nMissRate\n");
	for (i = 0; i < NUM_ROWS; i++) {

		if (i == 0) {
			printf("           ");
			for (k = 0; k < NUM_BLOCK; k++)
				printf("%s/%-4d                                 ", label, BLOCK_SIZES[k]);
			printf("\nI cache   ");
			for (k = 0; k < NUM_BLOCK; k++)
				for (j = 0; j < NUM_CACHE; j++) printf("%-7d", CACHE_SIZES[j]);
			printf("\n");
		}

		if (i == NUM_ASSOC) {
			printf("\n           ");
			for (k = 0; k < NUM_BLOCK; k++)
				printf("%s/%-4d                                 ", label, BLOCK_SIZES[k]);
			printf("\nD cache   ");
			for (k = 0; k < NUM_BLOCK; k++)
				for (j = 0; j < NUM_CACHE; j++) printf("%-7d", CACHE_SIZES[j]);
			printf("\n");
		}

		if (i % NUM_ASSOC == 0)      printf("Direct | ");
		else if (i % NUM_ASSOC == 1) printf("2  Way | ");
		else if (i % NUM_ASSOC == 2) printf("4  Way | ");
		else                         printf("8  Way | ");

		for (j = 0; j < NUM_COLS; j++)
			printf("%.4lf ", miss[i][j]);
		printf("\n");
	}

	printf("\nWrite Count\n");
	for (i = 0; i < NUM_ROWS; i++) {

		if (i == 0) {
			printf("           ");
			for (k = 0; k < NUM_BLOCK; k++)
				printf("%s/%-4d                          ", label, BLOCK_SIZES[k]);
			printf("\nI cache   ");
			for (k = 0; k < NUM_BLOCK; k++)
				for (j = 0; j < NUM_CACHE; j++)
					printf("%6d", CACHE_SIZES[j]);
			printf("\n");
		}

		if (i == NUM_ASSOC) {
			printf("\n           ");
			for (k = 0; k < NUM_BLOCK; k++)
				printf("%s/%-4d                          ", label, BLOCK_SIZES[k]);
			printf("\nD cache   ");
			for (k = 0; k < NUM_BLOCK; k++)
				for (j = 0; j < NUM_CACHE; j++)
					printf("%6d", CACHE_SIZES[j]);
			printf("\n");
		}

		if (i % NUM_ASSOC == 0)      printf("Direct | ");
		else if (i % NUM_ASSOC == 1) printf("2  Way | ");
		else if (i % NUM_ASSOC == 2) printf("4  Way | ");
		else                         printf("8  Way | ");

		for (j = 0; j < NUM_COLS; j++)
			printf("%5d ", writes[i][j]);

		printf("\n");
	}
}

static void print_best_results(
	const double lru_miss[NUM_ROWS][NUM_COLS], const int lru_writes[NUM_ROWS][NUM_COLS],
	const int lru_i_tot[NUM_ROWS][NUM_COLS], const int lru_d_tot[NUM_ROWS][NUM_COLS],
	const double fifo_miss[NUM_ROWS][NUM_COLS], const int fifo_writes[NUM_ROWS][NUM_COLS],
	const int fifo_i_tot[NUM_ROWS][NUM_COLS], const int fifo_d_tot[NUM_ROWS][NUM_COLS],
	int i_hit, int i_miss, int d_hit, int d_miss) {

	int cl;

	for (cl = 0; cl < NUM_CACHE; cl++) {

		double best_i_time = DBL_MAX;
		double best_d_time = DBL_MAX;

		const char* best_i_policy = "N/A";
		const char* best_d_policy = "N/A";

		int best_i_block = 0, best_i_assoc = 0;
		int best_d_block = 0, best_d_assoc = 0;

		double best_i_missrate = 0.0;
		double best_d_missrate = 0.0;
		int best_d_writes = 0;

		for (int b = 0; b < NUM_BLOCK; b++) {
			int block = BLOCK_SIZES[b];
			int col = col_idx(b, cl);

			for (int a = 0; a < NUM_ASSOC; a++) {
				int assoc = ASSOC_LIST[a];
				int r_i = row_i(a);
				int r_d = row_d(a);

				// I-cache 
				{
					int total = lru_i_tot[r_i][col];
					if (total > 0) {
						double mr = lru_miss[r_i][col];
						int miss_cnt = (int)(mr * (double)total + 0.5);
						int hit_cnt = total - miss_cnt;
						double cycles = (double)hit_cnt * (double)i_hit + (double)miss_cnt * (double)i_miss;
						if (cycles < best_i_time) {
							best_i_time = cycles;
							best_i_policy = "LRU";
							best_i_block = block;
							best_i_assoc = assoc;
							best_i_missrate = mr;
						}
					}
				}
				{
					int total = fifo_i_tot[r_i][col];
					if (total > 0) {
						double mr = fifo_miss[r_i][col];
						int miss_cnt = (int)(mr * (double)total + 0.5);
						int hit_cnt = total - miss_cnt;
						double cycles = (double)hit_cnt * (double)i_hit + (double)miss_cnt * (double)i_miss;
						if (cycles < best_i_time) {
							best_i_time = cycles;
							best_i_policy = "FIFO";
							best_i_block = block;
							best_i_assoc = assoc;
							best_i_missrate = mr;
						}
					}
				}

				// D-cache 
				{
					int total = lru_d_tot[r_d][col];
					if (total > 0) {
						double mr = lru_miss[r_d][col];
						int miss_cnt = (int)(mr * (double)total + 0.5);
						int hit_cnt = total - miss_cnt;
						int wb = lru_writes[r_d][col];
						double cycles = (double)hit_cnt * (double)d_hit + (double)miss_cnt * (double)d_miss + (double)wb * (double)d_miss;
						if (cycles < best_d_time) {
							best_d_time = cycles;
							best_d_policy = "LRU";
							best_d_block = block;
							best_d_assoc = assoc;
							best_d_missrate = mr;
							best_d_writes = wb;
						}
					}
				}
				{
					int total = fifo_d_tot[r_d][col];
					if (total > 0) {
						double mr = fifo_miss[r_d][col];
						int miss_cnt = (int)(mr * (double)total + 0.5);
						int hit_cnt = total - miss_cnt;
						int wb = fifo_writes[r_d][col];
						double cycles = (double)hit_cnt * (double)d_hit + (double)miss_cnt * (double)d_miss + (double)wb * (double)d_miss;
						if (cycles < best_d_time) {
							best_d_time = cycles;
							best_d_policy = "FIFO";
							best_d_block = block;
							best_d_assoc = assoc;
							best_d_missrate = mr;
							best_d_writes = wb;
						}
					}
				}
			}
		}

		printf("--- Cache Size: %d bytes ---\n", CACHE_SIZES[cl]);

		if (best_i_time == DBL_MAX)
			printf("  I-Cache: No instruction accesses.\n");
		else
			printf("  Best I-Cache: Policy=%-4s | Block=%-4d | Assoc=%-2d | MissRate=%.4f | Total Cycles=%.0f\n",
				best_i_policy, best_i_block, best_i_assoc, best_i_missrate, best_i_time);

		if (best_d_time == DBL_MAX)
			printf("  D-Cache: No data accesses.\n");
		else
			printf("  Best D-Cache: Policy=%-4s | Block=%-4d | Assoc=%-2d | MissRate=%.4f | Writes=%-5d | Total Cycles=%.0f\n",
				best_d_policy, best_d_block, best_d_assoc, best_d_missrate, best_d_writes, best_d_time);

		printf("\n");
	}
}

static void print_two_cache_state(const char* policy, int is_icache, int index, int assoc) {
	printf("\n[Cache State Dump] Policy=%s | %s | index=%d | assoc=%d\n",
		policy,
		is_icache ? "I-Cache" : "D-Cache",
		index, assoc);

	if (strcmp(policy, "LRU") == 0 || strcmp(policy, "lru") == 0) {
		struct Block_LRU* cache = is_icache ? icah_lru : dcah_lru;
		for (int i = 0; i < assoc; i++) {
			printf("  Way %-2d | valid=%d  tag=%lu  write_back=%d\n",
				i, cache[index].valid[i], cache[index].tag[i], cache[index].write_back[i]);
		}
	}
	else if (strcmp(policy, "FIFO") == 0 || strcmp(policy, "fifo") == 0) {
		struct Block_FIFO* cache = is_icache ? icah_fifo : dcah_fifo;
		int* p = is_icache ? icah_fifo_ptr : dcah_fifo_ptr;

		printf("  FIFO pointer = %d\n", p[index]);
		for (int i = 0; i < assoc; i++) {
			printf("  Way %-2d | valid=%d  tag=%lu  write_back=%d\n",
				i, cache[index].valid[i], cache[index].tag[i], cache[index].write_back[i]);
		}
	}
	printf("\n");
}

static void print_new_cache_state(int is_icache, int index, int assoc) {
	printf("\n[Cache State Dump] Policy=NEW(priority_counter) | %s | index=%d | assoc=%d\n",
		is_icache ? "I-Cache" : "D-Cache",
		index, assoc);

	struct Block_NEW* cache = is_icache ? icah_new : dcah_new;
	int* ptr = is_icache ? icah_new_ptr : dcah_new_ptr;

	if (!cache || !ptr) {
		printf("  (NEW cache not initialized)\n\n");
		return;
	}

	printf("  scan start(ptr) = %d\n", ptr[index]);
	for (int i = 0; i < assoc; i++) {
		printf("  Way %-2d | valid=%d  tag=%lu  write_back=%d  priority_counter=%u\n",
			i,
			cache[index].valid[i],
			cache[index].tag[i],
			cache[index].write_back[i],
			(unsigned)cache[index].priority_counter[i]);
	}
	printf("\n");
}

int main(int argc, char* argv[]) {
	if (argc < 3 || (argc > 3 && argc < 7) || argc > 7)
		usage(argv[0]);

	int policy = 0;
	int i_hit_c = 0, i_miss_c = 0, d_hit_c = 0, d_miss_c = 0;

	char* trace_file = NULL;

	if (!strcasecmp(argv[1], "FIFO")) {
		if (argc != 3) usage(argv[0]);
		policy = 1;
		trace_file = argv[2];
	}
	else if (!strcasecmp(argv[1], "LRU")) {
		if (argc != 3) usage(argv[0]);
		policy = 0;
		trace_file = argv[2];
	}
	else if (!strcasecmp(argv[1], "NEW")) {
		if (argc != 3) usage(argv[0]);
		policy = 3;
		trace_file = argv[2];
	}
	else if (!strcasecmp(argv[1], "BEST")) {
		if (argc != 7) usage(argv[0]);
		policy = 2;
		trace_file = argv[2];
		i_hit_c = atoi(argv[3]);
		i_miss_c = atoi(argv[4]);
		d_hit_c = atoi(argv[5]);
		d_miss_c = atoi(argv[6]);
	}
	else {
		usage(argv[0]);
	}

	int* type = NULL;
	unsigned long* addr = NULL;
	int length = 0;

	printf("Reading trace file: %s\n", trace_file);
	read_trace(trace_file, &type, &addr, &length);
	printf("Trace contains %d memory accesses.\n", length);

	if (policy == 0) {
		printf("Simulating LRU policy...\n");
		double miss[NUM_ROWS][NUM_COLS] = { {0} };
		int writes[NUM_ROWS][NUM_COLS] = { {0} };
		int i_tot[NUM_ROWS][NUM_COLS] = { {0} };
		int d_tot[NUM_ROWS][NUM_COLS] = { {0} };

		simulate_lru(type, addr, length, miss, writes, i_tot, d_tot);
		print_results("LRU", miss, writes);
	}
	else if (policy == 1) {
		printf("Simulating FIFO policy...\n");
		double miss[NUM_ROWS][NUM_COLS] = { {0} };
		int writes[NUM_ROWS][NUM_COLS] = { {0} };
		int i_tot[NUM_ROWS][NUM_COLS] = { {0} };
		int d_tot[NUM_ROWS][NUM_COLS] = { {0} };

		simulate_fifo(type, addr, length, miss, writes, i_tot, d_tot);
		print_results("FIFO", miss, writes);
	}
	else if (policy == 3) {
		printf("Simulating NEW policy...\n");
		double miss[NUM_ROWS][NUM_COLS] = { {0} };
		int writes[NUM_ROWS][NUM_COLS] = { {0} };
		int i_tot[NUM_ROWS][NUM_COLS] = { {0} };
		int d_tot[NUM_ROWS][NUM_COLS] = { {0} };

		simulate_new(type, addr, length, miss, writes, i_tot, d_tot);
		print_results("NEW", miss, writes);
	}
	else {
		printf("Simulating LRU policy for BEST...\n");
		double lru_miss[NUM_ROWS][NUM_COLS] = { {0} };
		int lru_writes[NUM_ROWS][NUM_COLS] = { {0} };
		int lru_i_tot[NUM_ROWS][NUM_COLS] = { {0} };
		int lru_d_tot[NUM_ROWS][NUM_COLS] = { {0} };

		double fifo_miss[NUM_ROWS][NUM_COLS] = { {0} };
		int fifo_writes[NUM_ROWS][NUM_COLS] = { {0} };
		int fifo_i_tot[NUM_ROWS][NUM_COLS] = { {0} };
		int fifo_d_tot[NUM_ROWS][NUM_COLS] = { {0} };

		simulate_lru(type, addr, length, lru_miss, lru_writes, lru_i_tot, lru_d_tot);
		printf("Simulating FIFO policy for BEST...\n");
		simulate_fifo(type, addr, length, fifo_miss, fifo_writes, fifo_i_tot, fifo_d_tot);

		printf("\n--- BEST Configuration Analysis ---\n");
		printf("Cycle Parameters: I(Hit/Miss) = %d/%d, D(Hit/Miss) = %d/%d\n\n",
			i_hit_c, i_miss_c, d_hit_c, d_miss_c);

		print_best_results(lru_miss, lru_writes, lru_i_tot, lru_d_tot,
			fifo_miss, fifo_writes, fifo_i_tot, fifo_d_tot,
			i_hit_c, i_miss_c, d_hit_c, d_miss_c);
	}

	free(type);
	free(addr);
	return 0;
}