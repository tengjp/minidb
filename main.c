#include "config.h"
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <error.h>
#include <sys/types.h>
#include <sys/stat.h>
//#include <wchar.h>
#include <inttypes.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <locale.h>
#include <time.h>

#include "readline.h"

struct stat st;
char* mmap_addr = 0;
char* mmap_addr_end = 0;
char* mmap_read_beg = 0;

int g_totalLineCnt;
int g_columnCnt;
char** g_heads;
unsigned char* g_columnType;

typedef enum QUERY_OP{
	EQ = 0, // equal
	GT = 1, // greater than
	LT = 2, // little than
	BT = 3,  // between
	OP_MAX

}QUERY_OP;
typedef struct FIELD_INFO{
	unsigned int start; // offset to the file start
	unsigned int end;
}FIELD_INFO;
int g_fieldInfoSize = sizeof(FIELD_INFO);
size_t g_fieldInfoPtrSize = sizeof(FIELD_INFO*);

// offset to file start of each line end
unsigned int* g_line_end;

// binary tree index
typedef union FIELD_VALUE{
	FIELD_INFO* strValueInfo;
	unsigned int intValue;
}FIELD_VALUE;
typedef struct INDEX_INFO{
	FIELD_VALUE fieldValue;
	unsigned int lineIdx;
}INDEX_INFO;
INDEX_INFO** g_index_info;
size_t g_idxInfoSize = sizeof(INDEX_INFO);

char *trimwhitespace(char *str)
{
	char *end;

	// Trim leading space
	while(isspace((unsigned char)*str)) str++;

	if(*str == 0)  // All spaces?
		return str;

	// Trim trailing space
	end = str + strlen(str) - 1;
	while(end > str && isspace((unsigned char)*end)) end--;

	// Write new null terminator
	*(end+1) = 0;

	return str;
}

// get line count
int getLineCnt(const char* file) {
	FILE *pf;
	char lineCntInfo[1024];
	char* cmdBuff = (char*)malloc(6 + strlen(file) + 1);
	memcpy(cmdBuff, "wc -l ", 6);
	memcpy(cmdBuff + 6, file, strlen(file) + 1);

	//printf("file: %s, wc cmd: %s\n", file, cmdBuff);
	pf = popen(cmdBuff, "r");
	fread(lineCntInfo, sizeof(lineCntInfo), 1, pf);

	//printf("line count info: %s", lineCntInfo);
	//printf("lineCnt: %d\n", lineCnt);

	pclose(pf);
	return atoi(lineCntInfo);
}

int allDecDigit(const char* s) {
	for ( const char* c = s; 0 != *c; ++c ) {
		if (*c < '0'  || *c > '9') {
			return 0;
		}
	}
	return 1;
}

#define MAX_COLUMN 256
int getColumnCnt(char* line) {
	if (0 == *line) {
		return 0;
	}
	int   cnt  = 0;
	char* pos  = 0;
	char* next = 0;

	for (pos = line, cnt = 0; *pos != 0; pos = next + 1) {
		next = strchr(pos, ',');

		++cnt;
		if (0 == next) {
			break;
		}
	}
	return cnt;
}

char** getRowFields(char* line, int* cnt) {
	char* pos = 0;
	char* next = 0;
	int   idx = 0;
	size_t size;
	if (0 == cnt) {
		return 0;
	}

	if (0 == *line) {
		return 0;
	}

	char** heads = (char**)malloc(sizeof(char*) * (*cnt));
	char** pheads = heads;
	for (pos = line, idx = 0; *pos != 0; pos = next + 1) {
		next = strchr(pos, ',');
		if (0 == next) {
			size = strlen(pos) + 1;
		}
		else {
			size = next - pos + 1;
		}
		pheads[idx] = (char*)malloc(size);
		memcpy(pheads[idx], pos, size);
		pheads[idx][size - 1] = 0;

		++idx;
		if (0 == next) {
			break;
		}
	}
	*cnt = idx;
	return heads;
}
FIELD_INFO** myParseCsvLine(char* line) {
	int idx = 0;
	char* pos = line;
	char* nextField = 0;

	size_t lineLen = strlen(line);
	if ('\n' == line[lineLen]) {
	}

	FIELD_INFO** info = (FIELD_INFO**)malloc(g_columnCnt * g_fieldInfoPtrSize);
	//printf("parse line: %s, last char: %c\n", line, line[lineLen - 1]);
	for (idx = 0, pos = line; idx < g_columnCnt; pos = nextField + 1, ++idx) {
		nextField = strchr(pos, ',');
		//printf("myParseCsv, field %d start: %s(%p, %p)\n", idx, pos, pos, mmap_addr);
		info[idx] = (FIELD_INFO*) malloc(g_fieldInfoSize);
		info[idx]->start = pos - mmap_addr;
		// it's the last field or the only field
		if (0 == nextField) {
			info[idx]->end = line + lineLen - 1 - mmap_addr;
			break;
		}
		else {
			info[idx]->end = nextField - 1 - mmap_addr;
		}
	}
	//info[idx + 1] = 0;
	return info;
}

void dumpIntIndex(int index) {
	INDEX_INFO* currentColumnIdx = g_index_info[index];
	printf("=====INDEX for column %d\n", index);
	for (int i = 0; i < g_totalLineCnt - 1; ++i) {
		printf("Column value: %u, line index: %d\n", (currentColumnIdx + i)->fieldValue.intValue, (currentColumnIdx + i)->lineIdx);
	}
	printf("=====INDEX for column %d END\n\n", index);
}
void dumpStrIndex(int index) {
	// column index head
	INDEX_INFO* currentColumnIdx = g_index_info[index];
	printf("=====INDEX for column %d\n", index);
	for (int i = 0; i < g_totalLineCnt - 1; ++i) {
		// index for a line
		INDEX_INFO* tmpIdxInfo = currentColumnIdx + i;
		char tmpChar = mmap_addr[tmpIdxInfo->fieldValue.strValueInfo->end + 1];
		mmap_addr[tmpIdxInfo->fieldValue.strValueInfo->end + 1] = 0;
		printf("Column value: %s, line index: %d\n", mmap_addr + tmpIdxInfo->fieldValue.strValueInfo->start, tmpIdxInfo->lineIdx);
		mmap_addr[tmpIdxInfo->fieldValue.strValueInfo->end + 1] = tmpChar;
	}
	printf("=====INDEX for column %d END\n\n", index);
}
void dumpIndice() {
	printf("\n=====================DUMP INDICE BEGIN================\n");
	for (int i = 0; i < g_columnCnt; ++i) {
		int columnType = g_columnType[i];
		if ( 0 == columnType) {
			dumpIntIndex(i);
		}
		else {
			dumpStrIndex(i);
		}
	}
	printf("\n=====================DUMP INDICE END=================\n");
}
typedef int (*FieldValueCmpFun) (const void*, const void*);
int intFieldValueCmp(const void* value1, const void* value2) {
	FIELD_VALUE* fValue1 = (FIELD_VALUE*)value1;
	FIELD_VALUE* fValue2 = (FIELD_VALUE*)value2;
	return fValue1->intValue > fValue2->intValue ? 1 : (fValue1->intValue < fValue2->intValue ? -1 : 0);
}
int strFieldValueCmp(const void* value1, const void* value2) {
	FIELD_VALUE* fValue1 = (FIELD_VALUE*)value1;
	FIELD_VALUE* fValue2 = (FIELD_VALUE*)value2;
	char tmpChar1 = mmap_addr[fValue1->strValueInfo->end + 1];
	char tmpChar2 = mmap_addr[fValue2->strValueInfo->end + 1];
	mmap_addr[fValue1->strValueInfo->end + 1] = 0;
	mmap_addr[fValue2->strValueInfo->end + 1] = 0;
	int result = strcmp(mmap_addr + fValue1->strValueInfo->start, mmap_addr + fValue2->strValueInfo->start);
	mmap_addr[fValue1->strValueInfo->end + 1] = tmpChar1;
	mmap_addr[fValue2->strValueInfo->end + 1] = tmpChar2;
	return result;
}
void HeapAdjust(INDEX_INFO* indexInfo, int start, int end, FieldValueCmpFun cmpFun) {
	INDEX_INFO tmpIdxInfo;
	memcpy(&tmpIdxInfo, indexInfo + start, g_idxInfoSize);
	// printf("\n\nHeapAdjust START, start %d, end %d\n", start, end);

	for (int i = 2 * start; i <= end; i *= 2) {
		if ( i < end &&
				cmpFun(&((indexInfo + i)->fieldValue), &((indexInfo + i + 1)->fieldValue)) < 0) {
			++i;
		}
		if (!(cmpFun(&(tmpIdxInfo.fieldValue), &((indexInfo + i)->fieldValue)) < 0)) break;
		memcpy(indexInfo + start, indexInfo + i, g_idxInfoSize);
		start = i;
	}
	memcpy(indexInfo + start, &tmpIdxInfo, g_idxInfoSize);
	// if (cmpFun == intFieldValueCmp) {
	//     printf("heap build finished, heap top item: value %d, line idx %d\n", indexInfo[tmpStart].fieldValue.intValue, indexInfo[tmpStart].lineIdx);
	// }
	// printf("HeapAdjust END\n\n");
}

void heapSortColumnIndex(int columnIdx) {
	int indexLen = g_totalLineCnt - 1;
	int i = 0;

	INDEX_INFO* currentColumnIdx = g_index_info[columnIdx];

	FieldValueCmpFun cmpFun = 0;
	if (0 == g_columnType[columnIdx]) {
		cmpFun = intFieldValueCmp;
	}
	else {
		cmpFun = strFieldValueCmp;
	}
	for (i = (indexLen - 1) / 2; i >= 0; --i) {
		HeapAdjust(currentColumnIdx, i, indexLen - 1, cmpFun);
	}
	for (i = indexLen - 1; i > 0; --i) {
		INDEX_INFO tmpIdxInfo;
		memcpy(&tmpIdxInfo, currentColumnIdx + i, g_idxInfoSize);
		memcpy(currentColumnIdx + i, currentColumnIdx, g_idxInfoSize);
		memcpy(currentColumnIdx, &tmpIdxInfo, g_idxInfoSize);
		HeapAdjust(currentColumnIdx, 0, i - 1, cmpFun);
	}
}

void buildIndice() {
	for (int i = 0; i < g_columnCnt; ++i) {
		heapSortColumnIndex(i);
	}
}

void
myqsort(void *aa, size_t n, size_t es, int (*cmp)(const void *, const void *));
void qsortColumnIndex(int columnIdx) {
	INDEX_INFO* currentColumnIdx = g_index_info[columnIdx];

	FieldValueCmpFun cmpFun = 0;
	if (0 == g_columnType[columnIdx]) {
		cmpFun = intFieldValueCmp;
	}
	else {
		cmpFun = strFieldValueCmp;
	}
	myqsort(currentColumnIdx, g_totalLineCnt - 1, g_idxInfoSize, cmpFun);
}
void buildIndiceQsort() {
	for (int i = 0; i < g_columnCnt; ++i) {
		qsortColumnIndex(i);
	}
}

int* g_searchResultLines;

int insertSearchIntEqual(INDEX_INFO* idxInfo, int value) {
	int low = 0;
	int high = g_totalLineCnt - 2;
	while (low <= high) {
		unsigned int lowValue = (idxInfo + low)->fieldValue.intValue;
		unsigned int highValue = (idxInfo + high)->fieldValue.intValue;

		if (low == high) {
			if (value == lowValue) {
				return low;
			}
			else {
				return -1;
			}
		}

		if (lowValue == highValue) {
			if ( value != lowValue) {
				return -1;
			}
			else {
				return low;
			}
		}
		if (value < lowValue || value > highValue) {
			return -1;
		}

		int mid = low + (value - lowValue) / (highValue - lowValue) * (high - low);
		unsigned int midValue = (idxInfo + mid)->fieldValue.intValue;
		if(midValue == value) return mid;
		if(midValue > value) high = mid - 1;
		if(midValue < value) low = mid + 1;
	}
	return -1;
}

// find the last item that's little than or equal to value
int insertSearchIntLt(INDEX_INFO* idxInfo, int value, int low, int high) {
	if (low > high) {
		return -1;
	}
	unsigned int lowValue = (idxInfo + low)->fieldValue.intValue;
	if (low == high) {
		if (lowValue <= value) {
			return high;
		}
		else {
			return -1;
		}
	}
	unsigned int highValue = (idxInfo + high)->fieldValue.intValue;
	if (highValue <= value) {
		return high;
	}
	// all are greater than value
	if (lowValue > value) {
		return -1;
	}
	int mid =  (high + low)/2;
	//int mid = low + (value - lowValue) / (highValue - lowValue) * (high - low);
	unsigned int midValue = (idxInfo + mid)->fieldValue.intValue;
	if(midValue <= value) {
		// continue to search
		if (mid == low) {
			return mid;
		}
		else {
			return insertSearchIntLt(idxInfo, value, mid, high);
		}
	}
	return insertSearchIntLt(idxInfo, value, low, mid - 1);
}

// find the first item that's greater than or equal to value
int insertSearchIntGt(INDEX_INFO* idxInfo, int value, int low, int high) {
	//printf("insertSearchIntGt, value %d, low %d, high %d\n", value, low, high);
	if (low > high) {
		return -1;
	}
	unsigned int lowValue = (idxInfo + low)->fieldValue.intValue;
	if (low == high) {
		if (lowValue >= value) {
			return high;
		}
		else {
			return -1;
		}
	}
	if (lowValue >= value) {
		return low;
	}
	unsigned int highValue = (idxInfo + high)->fieldValue.intValue;
	if (highValue < value) {
		return -1;
	}
	//if (highValue <= value) {
	//    return high;
	//}
	//if (lowValue > value) {
	//    return -1;
	//}
	int mid =  (high + low)/2;
	//int mid = low + (value - lowValue) / (highValue - lowValue) * (high - low);
	unsigned int midValue = (idxInfo + mid)->fieldValue.intValue;
	if(midValue >= value) {
		if (mid == high) {
			return mid;
		}
		else {
			return insertSearchIntGt(idxInfo, value, low, mid);
		}
	}
	return insertSearchIntGt(idxInfo, value, mid + 1, high);
}
// find the last item that's little than or equal to value
int insertSearchIntLtNonRecur(INDEX_INFO* idxInfo, int value) {
	int low = 0;
	int high = g_totalLineCnt - 2;
	while (low <= high) {
		unsigned int lowValue = (idxInfo + low)->fieldValue.intValue;
		unsigned int highValue = (idxInfo + high)->fieldValue.intValue;

		if (low == high) {
			if (lowValue <= value) {
				return high;
			}
			else {
				return -1;
			}
		}

		if (highValue <= value) {
			return high;
		}
		if (lowValue > value) {
			return -1;
		}
		int mid =  (high + low)/2;
		unsigned int midValue = (idxInfo + mid)->fieldValue.intValue;
		if(midValue <= value) {
			if (mid == low) {
				return mid;
			}
			else {
				low = mid;
			}
		}
		else {
			high = mid -1;
		}
	}
	return -1;
}

// find the first item that's greater than or equal to value
int insertSearchIntGtNonRecur(INDEX_INFO* idxInfo, int value) {
	int low = 0;
	int high = g_totalLineCnt - 2;
	while (low <= high) {
		unsigned int lowValue = (idxInfo + low)->fieldValue.intValue;
		unsigned int highValue = (idxInfo + high)->fieldValue.intValue;

		if (low == high) {
			if (lowValue >= value) {
				return high;
			}
			else {
				return -1;
			}
		}
		if (lowValue >= value) {
			return low;
		}
		if (highValue < value) {
			return -1;
		}
		int mid =  (high + low)/2;
		unsigned int midValue = (idxInfo + mid)->fieldValue.intValue;
		if(midValue >= value) {
			if (mid == high) {
				return mid;
			}
			else {
				high = mid;
			}
		}
		else {
			low = mid + 1;
		}
	}
	return -1;
}

void searchIntColumn(int columnIdx, int value1, int value2, QUERY_OP opType, int* matchCnt) {
	*matchCnt = 0;
	int cnt = 0;
	int matchIdx1, matchIdx2;
	INDEX_INFO* columnIdxInfo = g_index_info[columnIdx];
	memset(g_searchResultLines, 0, (g_totalLineCnt - 1) * sizeof(int));
	switch (opType) {
		case EQ:
			{
				matchIdx1 = insertSearchIntEqual(columnIdxInfo, value1);
				if (-1 != matchIdx1) {
					g_searchResultLines[cnt++] = (columnIdxInfo + matchIdx1)->lineIdx;
					int low = matchIdx1 - 1, high = matchIdx1 + 1;
					// there maybe a match range
					while (low >=0 && high <= (g_totalLineCnt - 2) &&
							(columnIdxInfo + low)->fieldValue.intValue == value1 &&
							(columnIdxInfo + high)->fieldValue.intValue == value1) {
						g_searchResultLines[cnt++] = (columnIdxInfo + low)->lineIdx;
						g_searchResultLines[cnt++] = (columnIdxInfo + high)->lineIdx;
						--low; ++high;
					}
					while (low >=0 && (columnIdxInfo + low)->fieldValue.intValue == value1) {
						g_searchResultLines[cnt++] = (columnIdxInfo + low)->lineIdx;
						--low;
					}
					while (high <= (g_totalLineCnt - 2) && (columnIdxInfo + high)->fieldValue.intValue == value1) {
						g_searchResultLines[cnt++] = (columnIdxInfo + high)->lineIdx;
						++high;
					}
				}
			}
			break;
		case GT:
			{
				// find the last item that little than value
				matchIdx1 = insertSearchIntLtNonRecur(columnIdxInfo, value1);
				//printf("query greater than, last line little or equal: %d\n", matchIdx1);
				// not items are little than value
				if (-1 == matchIdx1) {
					cnt = g_totalLineCnt - 1;
				}
				else {
					while (matchIdx1 + 1 < g_totalLineCnt - 1) {
						g_searchResultLines[cnt++] = (columnIdxInfo + matchIdx1 + 1)->lineIdx;
						++matchIdx1;
					}
				}
			}
			break;
		case LT:
			{
				// find the first item that greater than or equal to value
				matchIdx1 = insertSearchIntGtNonRecur(columnIdxInfo, value1);
				//printf("query little than, first line greater or equal: %d\n", matchIdx1);
				// not items are greater than value
				if (-1 == matchIdx1) {
					cnt = g_totalLineCnt - 1;
				}
				else {
					while (matchIdx1 - 1 >= 0) {
						g_searchResultLines[cnt++] = (columnIdxInfo + matchIdx1 - 1)->lineIdx;
						--matchIdx1;
					}
				}
			}
			break;
		case BT:
			// find the last item that's little than or equal to value1
			matchIdx1 = insertSearchIntLtNonRecur(columnIdxInfo, value1 - 1);
			// find the first item that's greater than or equal to value2
			matchIdx2 = insertSearchIntGtNonRecur(columnIdxInfo, value2 + 1);
			if (-1 == matchIdx2) {
				matchIdx2 = g_totalLineCnt - 1;
			}
			while (matchIdx1 + 1 <= matchIdx2 - 1) {
				g_searchResultLines[cnt++] = (columnIdxInfo + matchIdx1 + 1)->lineIdx;
				++matchIdx1;
			}
			break;
		default:
			break;
	}
	*matchCnt = cnt;
}

int insertSearchStrEqual(INDEX_INFO* idxInfo, char* value) {
	int low = 0;
	int high = g_totalLineCnt - 2;
	while (low <= high) {
		char* lowValue = mmap_addr + (idxInfo + low)->fieldValue.strValueInfo->start;
		int end1 = (idxInfo + low)->fieldValue.strValueInfo->end + 1;
		char tmpChar1 = mmap_addr[end1];
		mmap_addr[end1] = 0;
		if (low == high) {
			if (0 == strcmp(value, lowValue)) {
				mmap_addr[end1] = tmpChar1;
				return low;
			}
			else {
				mmap_addr[end1] = tmpChar1;
				return -1;
			}
		}

		char* highValue = mmap_addr + (idxInfo + high)->fieldValue.strValueInfo->start;
		int end2 = (idxInfo + high)->fieldValue.strValueInfo->end + 1;
		char tmpChar2 = mmap_addr[end2];
		mmap_addr[end2] = 0;
		if (0 == strcmp(lowValue, highValue)) {
			if ( 0 != strcmp(value, highValue)) {
				mmap_addr[end1] = tmpChar1;
				mmap_addr[end2] = tmpChar2;
				return -1;
			}
			else {
				mmap_addr[end1] = tmpChar1;
				mmap_addr[end2] = tmpChar2;
				return low;
			}
		}
		if (strcmp(value, lowValue) < 0 || strcmp(value, highValue) > 0) {
			mmap_addr[end1] = tmpChar1;
			mmap_addr[end2] = tmpChar2;
			return -1;
		}
		mmap_addr[end1] = tmpChar1;
		mmap_addr[end2] = tmpChar2;

		int mid = (low + high) / 2;
		char* midValue = mmap_addr + (idxInfo + mid)->fieldValue.strValueInfo->start;
		int end3 = (idxInfo + mid)->fieldValue.strValueInfo->end + 1;
		char tmpChar3 = mmap_addr[end3];
		mmap_addr[end3] = 0;
		if(0 == strcmp(midValue, value)) {
			mmap_addr[end3] = tmpChar3;
			return mid;
		}
		else if (strcmp(midValue, value) > 0) {
			high = mid - 1;
		}
		else {
			low = mid + 1;
		}
		mmap_addr[end3] = tmpChar3;
	}
	return -1;

}

// find the last item that's little than or equal to value
int insertSearchStrLt(INDEX_INFO* idxInfo, char* value, int low, int high) {
	if (low > high) {
		return -1;
	}
	char* lowValue = mmap_addr + (idxInfo + low)->fieldValue.strValueInfo->start;
	int end1 = (idxInfo + low)->fieldValue.strValueInfo->end + 1;
	char tmpChar1 = mmap_addr[end1];
	mmap_addr[end1] = 0;
	if (low == high) {
		if (strcmp(lowValue, value) <= 0) {
			mmap_addr[end1] = tmpChar1;
			return high;
		}
		else {
			mmap_addr[end1] = tmpChar1;
			return -1;
		}
	}
	mmap_addr[end1] = tmpChar1;

	char* highValue = mmap_addr + (idxInfo + high)->fieldValue.strValueInfo->start;
	int end2 = (idxInfo + high)->fieldValue.strValueInfo->end + 1;
	char tmpChar2 = mmap_addr[end2];
	mmap_addr[end2] = 0;
	if (strcmp(highValue, value) <= 0) {
		mmap_addr[end2] = tmpChar2;
		return high;
	}
	if (strcmp(lowValue, value) > 0) {
		mmap_addr[end2] = tmpChar2;
		return -1;
	}
	mmap_addr[end2] = tmpChar2;

	int mid =  (high + low)/2;
	char* midValue = mmap_addr + (idxInfo + mid)->fieldValue.strValueInfo->start;
	int end3 = (idxInfo + mid)->fieldValue.strValueInfo->end + 1;
	char tmpChar3 = mmap_addr[end3];
	mmap_addr[end3] = 0;
	if(strcmp(midValue, value) <= 0) {
		if (mid == low) {
			mmap_addr[end3] = tmpChar3;
			return mid;
		}
		else {
			mmap_addr[end3] = tmpChar3;
			return insertSearchStrLt(idxInfo, value, mid, high);
		}
	}
	mmap_addr[end3] = tmpChar3;
	return insertSearchStrLt(idxInfo, value, low, mid - 1);
}

// find the first item that's greater than or equal to value
int insertSearchStrGt(INDEX_INFO* idxInfo, char* value, int low, int high) {
	if (low > high) {
		return -1;
	}
	char* lowValue = mmap_addr + (idxInfo + low)->fieldValue.strValueInfo->start;
	int end1 = (idxInfo + low)->fieldValue.strValueInfo->end + 1;
	char tmpChar1 = mmap_addr[end1];
	mmap_addr[end1] = 0;
	if (low == high) {
		if (strcmp(lowValue, value) >= 0) {
			mmap_addr[end1] = tmpChar1;
			return high;
		}
		else {
			mmap_addr[end1] = tmpChar1;
			return -1;
		}
	}
	if (strcmp(lowValue, value) >= 0) {
		mmap_addr[end1] = tmpChar1;
		return low;
	}
	mmap_addr[end1] = tmpChar1;

	char* highValue = mmap_addr + (idxInfo + high)->fieldValue.strValueInfo->start;
	int end2 = (idxInfo + high)->fieldValue.strValueInfo->end + 1;
	char tmpChar2 = mmap_addr[end2];
	mmap_addr[end2] = 0;
	if (strcmp(highValue, value) < 0) {
		mmap_addr[end2] = tmpChar2;
		return -1;
	}
	mmap_addr[end2] = tmpChar2;

	int mid =  (high + low)/2;
	char* midValue = mmap_addr + (idxInfo + mid)->fieldValue.strValueInfo->start;
	int end3 = (idxInfo + mid)->fieldValue.strValueInfo->end + 1;
	char tmpChar3 = mmap_addr[end3];
	mmap_addr[end3] = 0;
	if(strcmp(midValue, value) >= 0) {
		if (mid == high) {
			mmap_addr[end3] = tmpChar3;
			return mid;
		}
		else {
			mmap_addr[end3] = tmpChar3;
			return insertSearchStrGt(idxInfo, value, low, mid);
		}
	}
	mmap_addr[end3] = tmpChar3;
	return insertSearchStrGt(idxInfo, value, mid + 1, high);
}

// find the last item that's little than or equal to value
int insertSearchStrLtNonRecur(INDEX_INFO* idxInfo, char* value) {
	int low = 0;
	int high = g_totalLineCnt - 2;
	while (low <= high) {
		char* lowValue = mmap_addr + (idxInfo + low)->fieldValue.strValueInfo->start;
		int end1 = (idxInfo + low)->fieldValue.strValueInfo->end + 1;
		char tmpChar1 = mmap_addr[end1];
		mmap_addr[end1] = 0;
		if (low == high) {
			if (strcmp(lowValue, value) <= 0) {
				mmap_addr[end1] = tmpChar1;
				return high;
			}
			else {
				mmap_addr[end1] = tmpChar1;
				return -1;
			}
		}
		mmap_addr[end1] = tmpChar1;

		char* highValue = mmap_addr + (idxInfo + high)->fieldValue.strValueInfo->start;
		int end2 = (idxInfo + high)->fieldValue.strValueInfo->end + 1;
		char tmpChar2 = mmap_addr[end2];
		mmap_addr[end2] = 0;
		if (strcmp(highValue, value) <= 0) {
			mmap_addr[end2] = tmpChar2;
			return high;
		}
		if (strcmp(lowValue, value) > 0) {
			mmap_addr[end2] = tmpChar2;
			return -1;
		}
		mmap_addr[end2] = tmpChar2;

		int mid =  (high + low)/2;
		char* midValue = mmap_addr + (idxInfo + mid)->fieldValue.strValueInfo->start;
		int end3 = (idxInfo + mid)->fieldValue.strValueInfo->end + 1;
		char tmpChar3 = mmap_addr[end3];
		mmap_addr[end3] = 0;
		if(strcmp(midValue, value) <= 0) {
			if (mid == low) {
				mmap_addr[end3] = tmpChar3;
				return mid;
			}
			else {
				mmap_addr[end3] = tmpChar3;
				low = mid;
			}
		}
		else {
			high = mid - 1;
		}
		mmap_addr[end3] = tmpChar3;
	}
	return -1;
}

// find the first item that's greater than or equal to value
int insertSearchStrGtNonRecur(INDEX_INFO* idxInfo, char* value) {
	int low = 0;
	int high = g_totalLineCnt - 2;
	while (low <= high) {
		char* lowValue = mmap_addr + (idxInfo + low)->fieldValue.strValueInfo->start;
		int end1 = (idxInfo + low)->fieldValue.strValueInfo->end + 1;
		char tmpChar1 = mmap_addr[end1];
		mmap_addr[end1] = 0;
		if (low == high) {
			if (strcmp(lowValue, value) >= 0) {
				mmap_addr[end1] = tmpChar1;
				return high;
			}
			else {
				mmap_addr[end1] = tmpChar1;
				return -1;
			}
		}
		if (strcmp(lowValue, value) >= 0) {
			mmap_addr[end1] = tmpChar1;
			return low;
		}
		mmap_addr[end1] = tmpChar1;

		char* highValue = mmap_addr + (idxInfo + high)->fieldValue.strValueInfo->start;
		int end2 = (idxInfo + high)->fieldValue.strValueInfo->end + 1;
		char tmpChar2 = mmap_addr[end2];
		mmap_addr[end2] = 0;
		if (strcmp(highValue, value) < 0) {
			mmap_addr[end2] = tmpChar2;
			return -1;
		}
		mmap_addr[end2] = tmpChar2;

		int mid =  (high + low)/2;
		char* midValue = mmap_addr + (idxInfo + mid)->fieldValue.strValueInfo->start;
		int end3 = (idxInfo + mid)->fieldValue.strValueInfo->end + 1;
		char tmpChar3 = mmap_addr[end3];
		mmap_addr[end3] = 0;
		if(strcmp(midValue, value) >= 0) {
			if (mid == high) {
				mmap_addr[end3] = tmpChar3;
				return mid;
			}
			else {
				mmap_addr[end3] = tmpChar3;
				high = mid;
			}
		}
		else {
			low = mid + 1;
		}
		mmap_addr[end3] = tmpChar3;
	}
	return -1;

}

void searchStrColumn(int columnIdx, char* value1, char* value2, QUERY_OP opType, int* matchCnt) {
	*matchCnt = 0;
	int cnt = 0;
	int matchIdx1, matchIdx2;
	INDEX_INFO* columnIdxInfo = g_index_info[columnIdx];
	memset(g_searchResultLines, 0, (g_totalLineCnt - 1) * sizeof(int));
	switch (opType) {
		case EQ:
			{
				matchIdx1 = insertSearchStrEqual(columnIdxInfo, value1);
				if (-1 != matchIdx1) {
					g_searchResultLines[cnt++] = (columnIdxInfo + matchIdx1)->lineIdx;
					int low = matchIdx1 - 1, high = matchIdx1 + 1;
					// there maybe a match range
					while (low >=0 && high <= (g_totalLineCnt - 2)){ 
						char* lowValue = mmap_addr + (columnIdxInfo + low)->fieldValue.strValueInfo->start;
						int end1 = (columnIdxInfo + low)->fieldValue.strValueInfo->end + 1;
						char tmpChar1 = mmap_addr[end1];
						mmap_addr[end1] = 0;
						char* highValue = mmap_addr + (columnIdxInfo + high)->fieldValue.strValueInfo->start;
						int end2 = (columnIdxInfo + high)->fieldValue.strValueInfo->end + 1;
						char tmpChar2 = mmap_addr[end2];
						mmap_addr[end2] = 0;
						if ( 0 == strcmp(value1, lowValue) && 0 == strcmp(value1, highValue)) {
							g_searchResultLines[cnt++] = (columnIdxInfo + low)->lineIdx;
							g_searchResultLines[cnt++] = (columnIdxInfo + high)->lineIdx;
							--low; ++high;
							mmap_addr[end1] = tmpChar1;
							mmap_addr[end2] = tmpChar2;
						}
						else {
							mmap_addr[end1] = tmpChar1;
							mmap_addr[end2] = tmpChar2;
							break;
						}
					}
					while (low >= 0) {
						char* lowValue = mmap_addr + (columnIdxInfo + low)->fieldValue.strValueInfo->start;
						int end = (columnIdxInfo + low)->fieldValue.strValueInfo->end + 1;
						char tmpChar = mmap_addr[end];
						mmap_addr[end] = 0;
						if ( 0 == strcmp(value1, lowValue)) {
							g_searchResultLines[cnt++] = (columnIdxInfo + low)->lineIdx;
							--low;
							mmap_addr[end] = tmpChar;
						}
						else {
							mmap_addr[end] = tmpChar;
							break;
						}
					}
					while (high <= (g_totalLineCnt - 2)) {
						char* highValue = mmap_addr + (columnIdxInfo + high)->fieldValue.strValueInfo->start;
						int end = (columnIdxInfo + high)->fieldValue.strValueInfo->end + 1;
						char tmpChar = mmap_addr[end];
						mmap_addr[end] = 0;
						if ( 0 == strcmp(value1, highValue)) {
							g_searchResultLines[cnt++] = (columnIdxInfo + high)->lineIdx;
							mmap_addr[end] = tmpChar;
							++high;
						}
						else {
							mmap_addr[end] = tmpChar;
							break;
						}
					}
				}
			}
			break;
		case GT:
			{
				// find the last item that little than value
				matchIdx1 = insertSearchStrLtNonRecur(columnIdxInfo, value1);
				//printf("query greater than, last line little or equal: %d\n", matchIdx1);
				// not items are little than value
				if (-1 == matchIdx1) {
					cnt = g_totalLineCnt - 1;
				}
				else {
					while (matchIdx1 + 1 < g_totalLineCnt - 1) {
						g_searchResultLines[cnt++] = (columnIdxInfo + matchIdx1 + 1)->lineIdx;
						++matchIdx1;
					}
				}
			}
			break;
		case LT:
			{
				// find the first item that greater than or equal to value
				matchIdx1 = insertSearchStrGtNonRecur(columnIdxInfo, value1);
				//printf("query little than, first line greater or equal: %d\n", matchIdx1);
				// not items are greater than value
				if (-1 == matchIdx1) {
					cnt = g_totalLineCnt - 1;
				}
				else {
					while (matchIdx1 - 1 >= 0) {
						g_searchResultLines[cnt++] = (columnIdxInfo + matchIdx1 - 1)->lineIdx;
						--matchIdx1;
					}
				}
			}
			break;
		case BT:
			// find the last item that's little than or equal to value1
			matchIdx1 = insertSearchStrLtNonRecur(columnIdxInfo, value1);
			if (-1 == matchIdx1) {
				matchIdx1 = 0;
			}
			if (-1 != matchIdx1) {
				char* matchValue1 = mmap_addr + (columnIdxInfo + matchIdx1)->fieldValue.strValueInfo->start;
				int end1 = (columnIdxInfo + matchIdx1)->fieldValue.strValueInfo->end + 1;
				char tmpChar1 = mmap_addr[end1];
				mmap_addr[end1] = 0;
				if (strcmp(matchValue1, value1) < 0) {
					matchIdx1 += 1;
				}
				mmap_addr[end1] = tmpChar1;
			}
			// find the first item that's greater than or equal to value2
			matchIdx2 = insertSearchStrGtNonRecur(columnIdxInfo, value2);
			if (-1 == matchIdx2) {
				matchIdx2 = g_totalLineCnt - 2;
			}
			if (-1 != matchIdx2) {
				char* matchValue2 = mmap_addr + (columnIdxInfo + matchIdx2)->fieldValue.strValueInfo->start;
				int end2 = (columnIdxInfo + matchIdx2)->fieldValue.strValueInfo->end + 1;
				char tmpChar2 = mmap_addr[end2];
				mmap_addr[end2] = 0;
				if (strcmp(matchValue2, value2) > 0) {
					matchIdx2 -= 1;
				}
				mmap_addr[end2] = tmpChar2;
			}
			while (matchIdx1 <= matchIdx2) {
				g_searchResultLines[cnt++] = (columnIdxInfo + matchIdx1)->lineIdx;
				++matchIdx1;
			}
			break;
		default:
			break;
	}
	*matchCnt = cnt;
}

// return:
// 0: int, 1: string, -1: neither
int valueType(const char* value) {
	size_t len = strlen(value);
	if ('"' == value[0]) {
		if('"' == value[len - 1]) {
			//printf("%s is string.\n", value);
			return 1;
		}
		else {
			//printf("%s is invalid.\n", value);
			return -1;
		}
	}
	else {
		if('"' != value[len - 1]) {
			//printf("%s is int.\n", value);
			return 0;
		}
		else {
			//printf("%s is invalid.\n", value);
			return -1;
		}
	}
}

void printHead() {
	char tmpChar = mmap_addr[g_line_end[0] + 1];
	mmap_addr[g_line_end[0] + 1] = 0;
	printf("%s", mmap_addr);
	mmap_addr[g_line_end[0] + 1] = tmpChar;
}

void printMatchLines(int cnt) {
	char tmpChar;
	int lineIdx;
	if (cnt > 0) {
		printHead();
	}
	// all lines match
	if (cnt == g_totalLineCnt - 1) {
		for(int l = 1; l < cnt; ++l) {
			tmpChar = mmap_addr[g_line_end[l] + 1];
			mmap_addr[g_line_end[l] + 1] = 0;
			if ('\n' == mmap_addr[g_line_end[l]]) {
				printf("%s", mmap_addr + g_line_end[l - 1] + 1);
			}
			else{
				printf("%s\n", mmap_addr + g_line_end[l - 1] + 1);
			}
			mmap_addr[g_line_end[l] + 1] = tmpChar;
			if (l % 20 >= 19 && l < cnt && (cnt - l - 1 > 0)) {
				printf("%d more, press SPACE or ENTER to show, press any other key to stop\n", cnt - l - 1);
				char c = fgetc(stdin);
				printf("input %d\n", c);
				if (' ' != c && '\n' != c) {
					break;
				}
			}
		}
	}
	else {
		for(int l = 0; l < cnt; ++l) {
			lineIdx = g_searchResultLines[l];
			tmpChar = mmap_addr[g_line_end[lineIdx] + 1];
			mmap_addr[g_line_end[lineIdx] + 1] = 0;
			if ('\n' == mmap_addr[g_line_end[lineIdx]]) {
				printf("%s", mmap_addr + g_line_end[lineIdx - 1] + 1);
			}
			else{
				printf("%s\n", mmap_addr + g_line_end[lineIdx - 1] + 1);
			}
			mmap_addr[g_line_end[lineIdx] + 1] = tmpChar;
			if (l % 20 >= 19 && l < cnt && (cnt - l - 1 > 0)) {
				printf("%d more, press ENTER to show more, press any other key to stop\n", cnt - l - 1);
				char c;
				int n = read(STDIN_FILENO, &c, 1);
				printf("input %d\n", c);
				if (' ' != c && '\n' != c) {
					break;
				}
			}
		}
	}
}

int init(const char* file) {
	g_totalLineCnt = getLineCnt(file);
	if (g_totalLineCnt < 2) { // no data lines
		return -1;
	}
	g_searchResultLines = (int*)malloc((g_totalLineCnt - 1) * sizeof(int));

	int oflag = (O_RDONLY | O_NOCTTY);
	int fd = open(file, oflag);
	if (fd < 0)
	{
		printf("Failed open file %s\n", file);
		exit(-1);
	}

	if (fstat (fd, &st) != 0)
	{
		printf("fstat.\n");
		close(fd);
		exit(-1);
	}

	mmap_addr = (char*)mmap(0, st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (mmap_addr == MAP_FAILED) {
		printf("Failed mmap file.\n");
		close(fd);
		exit(-1);
	}
	mmap_read_beg = mmap_addr;
	mmap_addr_end = mmap_addr + st.st_size;
	//*mmap_addr_end = 0;

	return 0;
}

void initDataIndex(char* pRowHeadEnd) {
	char* lastRowEnd = pRowHeadEnd;
	char* rowEnd;
	char tmpChar;
	// data row start index
	int lineIdx = 1;
	while (0 != lastRowEnd && lastRowEnd + 1 < mmap_addr_end) {
		rowEnd = strchr(lastRowEnd + 1, '\n');
		if (0 != rowEnd) {
			*rowEnd = 0;
		}
		else {
			rowEnd = mmap_addr_end;
		}

		FIELD_INFO** rowData = myParseCsvLine(lastRowEnd + 1);

		g_line_end[lineIdx] = rowEnd - mmap_addr;

		//init each column index info of the current line
		for (int ci = 0; ci < g_columnCnt; ++ci) {
			// head line is no included in index
			INDEX_INFO* columnLineIndex = g_index_info[ci] + lineIdx - 1;
			columnLineIndex->lineIdx = lineIdx;
			if (0 == g_columnType[ci]) { // int
				tmpChar = mmap_addr[rowData[ci]->end + 1];
				mmap_addr[rowData[ci]->end + 1] = 0;
				unsigned int valueInt = atoi(mmap_addr + rowData[ci]->start);
				mmap_addr[rowData[ci]->end + 1] = tmpChar;
				columnLineIndex->fieldValue.intValue = valueInt;
			}
			else {
				columnLineIndex->fieldValue.strValueInfo = rowData[ci];
			}
		}
		if (0 != rowEnd) {
			*rowEnd = '\n';
		}
		lastRowEnd = rowEnd;
		++lineIdx;
	}
}

QUERY_OP getOpType(char* trimmedInput, char** opStartPos, char** andPos) {
	QUERY_OP opType = OP_MAX;
	char* tmpOpStartPos = strchr(trimmedInput, '=');
	char* tmpAndPos = 0;

	if (0 == tmpOpStartPos) {
		tmpOpStartPos = strchr(trimmedInput, '>');
		if (0 == tmpOpStartPos) {
			tmpOpStartPos = strchr(trimmedInput, '<');
			if (0 == tmpOpStartPos) {
				// name between a and b
				tmpOpStartPos = strstr(trimmedInput, "between");
				if (0 != tmpOpStartPos && isblank(tmpOpStartPos[7])) {
					tmpAndPos = strstr(tmpOpStartPos + 7, "and");
					if (0 != tmpAndPos && isblank(tmpAndPos[3])) {
						opType = BT;
					}
				}
			}
			else {
				opType = LT;
			}
		}
		else {
			opType = GT;
		}
	}
	else {
		opType = EQ;
	}
	*opStartPos = tmpOpStartPos;
	*andPos = tmpAndPos;
	return opType;
}

void mainLoop() {
	char* prompt = "minidb$ ";
	char* userInput = 0;
	char tmpChar;
	int index = 0;

	while (userInput = readline(prompt)) {
		char* field = 0;
		char* value1 = 0;
		char* value1Trimmed = 0;
		char* value2 = 0;
		char* value2Trimmed = 0;

		char* trimmedInput = trimwhitespace(userInput);
		if (0 == *trimmedInput) {
			free(userInput);
			printf("null input.\n");
			continue;
		}
		int trimmedInputLen = strlen(trimmedInput);
		char* opStartPos = 0;
		char* andPos = 0;
		QUERY_OP opType = getOpType(trimmedInput, &opStartPos, &andPos);
		if (OP_MAX == opType || (opStartPos == trimmedInput + trimmedInputLen - 1)) {
			printf("Invalid query.\n");
			continue;
		}

		int fieldSize = opStartPos - trimmedInput + 1;
		field = (char*)malloc(fieldSize);
		memcpy(field, trimmedInput, fieldSize);
		field[fieldSize - 1] = 0;
		char* fieldTrimmed = trimwhitespace(field);
		index = 0;
		int columnIdx = -1;
		for (char* pfield= g_heads[index]; index < g_columnCnt; pfield = g_heads[++index]) {
			if (0 == strcmp(fieldTrimmed, pfield)) {
				columnIdx = index;
				break;
			}
		}
		if (-1 == columnIdx) {
			printf("No field: %s\n", fieldTrimmed);
			goto next_round;
		}

		int valueTypeExpect = g_columnType[columnIdx];

		//printf("opType %d\n", opType);
		switch (opType) {
			case EQ:
			case GT:
			case LT:
				{
					int value1Size = trimmedInput + trimmedInputLen - opStartPos;
					value1 = (char*)malloc(value1Size);
					memcpy(value1, opStartPos + 1, value1Size);
					value1[value1Size - 1] = 0;
					value1Trimmed = trimwhitespace(value1);
					int value1TrimmedLen = strlen(value1Trimmed);
					int value1Type = valueType(value1Trimmed);
					if (value1Type != valueTypeExpect) {
						printf("Invalid query value.\n");
						goto next_round;
					}
					//printf("Query operation: %c, field:%s, value1:%s\n", *opStartPos, fieldTrimmed, value1Trimmed);
					if (0 == value1Type) { // int value
						if (!allDecDigit(value1Trimmed)) {
							printf("Query value is not integer.\n");
							goto next_round;
						}
						unsigned int int1Value = atoi(value1Trimmed);
						int cnt = 0;
						searchIntColumn(columnIdx, int1Value, -1, opType, &cnt);
						printMatchLines(cnt);
					}
					else { // string value
						int cnt = 0;
						value1Trimmed[value1TrimmedLen - 1] = 0;
						searchStrColumn(columnIdx, value1Trimmed + 1, 0, opType, &cnt);
						printMatchLines(cnt);
					}
				}
				break;
			case BT:
				{
					tmpChar = *andPos;
					// name between a and b
					int value1Size = andPos - (opStartPos + 7) + 1;
					value1 = (char*)malloc(value1Size);
					memcpy(value1, opStartPos + 7, value1Size);
					value1[value1Size - 1] = 0;
					value1Trimmed = trimwhitespace(value1);
					if (0 == strlen(value1Trimmed)) {
						printf("Invalid query value.\n");
						goto next_round;
					}
					int value1Type = valueType(value1Trimmed);
					if (value1Type != valueTypeExpect) {
						printf("Invalid query value.\n");
						goto next_round;
					}
					int value1TrimmedLen = strlen(value1Trimmed);

					int value2Size =  trimmedInput + trimmedInputLen - (andPos + 3) + 1;
					value2 = (char*)malloc(value2Size);
					memcpy(value2, andPos + 3, value2Size);
					value2[value2Size - 1] = 0;
					value2Trimmed = trimwhitespace(value2);
					int value2TrimmedLen = strlen(value2Trimmed);
					int value2Type = valueType(value2Trimmed);
					if (value2Type != valueTypeExpect) {
						printf("Invalid query value.\n");
						goto next_round;
					}

					if (0 == value1Type) {
						if (!allDecDigit(value1Trimmed) || !allDecDigit(value2Trimmed)) {
							printf("Query value is not integer.\n");
							goto next_round;
						}
						unsigned int int1Value = atoi(value1Trimmed);
						unsigned int int2Value = atoi(value2Trimmed);
						if (int1Value > int2Value) {
							printf("Invalid query.\n");
							goto next_round;
						}
						int cnt = 0;
						searchIntColumn(columnIdx, int1Value, int2Value, opType, &cnt);
						printMatchLines(cnt);
					}
					else {
						int cnt = 0;
						value1Trimmed[value1TrimmedLen - 1] = 0;
						value2Trimmed[value2TrimmedLen - 1] = 0;
						if (strcmp(value1Trimmed, value2Trimmed) > 0) {
							printf("Invalid query.\n");
							goto next_round;
						}
						if (strcmp(value1Trimmed, value2Trimmed) == 0) {
							opType = EQ;
						}
						searchStrColumn(columnIdx, value1Trimmed + 1, value2Trimmed + 1, opType, &cnt);
						printMatchLines(cnt);
					}
				}
				break;
			default:
				printf("op error.\n");
				break;
		} // switch end
next_round:
		free(field);
		free(value1);
		free(value2);
		free(userInput);
		userInput = 0;
	}
	//printf("EOF\n");
}

int main(int argc, char** argv) {
	printf("READY\n");
	if (argc < 2) {
		printf("Usage: a.out file.\n");
		exit(0);
	}

	setlocale(LC_ALL, "en_US.UTF-8");
	if (0 != init(argv[1])) {
		printf("No data lines.\n");
		exit(0);
	}
	//printf("file end: %c, %d\n", *mmap_addr_end, *mmap_addr_end);

	char* pRowHeadEnd = strchr(mmap_addr, '\n'); 
	if (0 == pRowHeadEnd) {
		printf("No data lines.\n");
		exit(0);
	}

	// heads info
	char tmpChar = *pRowHeadEnd;
	*pRowHeadEnd = 0;

	g_columnCnt = getColumnCnt(mmap_addr);
	g_heads = getRowFields(mmap_addr, &g_columnCnt);
	int index = 0;
	char* pfield = 0;
	// for (pfield= g_heads[index]; index < g_columnCnt; pfield = g_heads[++index]) {
	//     printf("Field name: %s\n", pfield);
	// }
	*pRowHeadEnd = tmpChar;

	char* pFirstDataRowEnd = strchr(pRowHeadEnd + 1, '\n');
	if (0 == pFirstDataRowEnd) {
		printf("No data lines.\n");
		exit(0);
	}

	//printf("Line cnt: %d, column cnt: %d, file size: %u\n", totalLineCnt, g_columnCnt, st.st_size);
	//printf("column cnt: %d\n", g_columnCnt);
	//printf("is using utf8: %d\n", is_using_utf8());

	g_columnType = (unsigned char*)malloc(g_columnCnt * sizeof(unsigned char));
	memset(g_columnType, 0, g_columnCnt * sizeof(unsigned char));

	// get data types for each column
	*pFirstDataRowEnd = 0;
	int cnt = g_columnCnt;
	char ** firstRowData = getRowFields(pRowHeadEnd + 1, &cnt);
	for (index = 0, pfield = firstRowData[index]; index < g_columnCnt; pfield = firstRowData[++index]) {
		if (!allDecDigit(pfield)) {
			g_columnType[index] = 1; // string
		}
	}
	*pFirstDataRowEnd = '\n';

	int lineIdx = 0;

	g_line_end = (unsigned int*)malloc(g_totalLineCnt * sizeof(unsigned int));
	// head line
	g_line_end[lineIdx] = pRowHeadEnd - mmap_addr;
	++lineIdx;

	// each column has seperate index
	g_index_info = (INDEX_INFO**)malloc(g_columnCnt * sizeof(INDEX_INFO*));
	for (int aa = 0; aa < g_columnCnt; ++aa) {
		g_index_info[aa] = (INDEX_INFO*)malloc((g_totalLineCnt - 1) * g_idxInfoSize);
	}

	//time_t initDataStart = time(NULL);
	//printf("Reading data start %d...\n", (int)initDataStart);
	initDataIndex(pRowHeadEnd);
	//time_t initDataEnd = time(NULL);
	//printf("Reading data end %d(%ds)...\n", (int)initDataEnd, (int)(initDataEnd - initDataStart));

	//time_t idxStartTime = time(NULL);
	//printf("Building indice start %d...\n", (int)idxStartTime);
	buildIndiceQsort();
	//buildIndice();
	//time_t idxEndTime = time(NULL);
	//printf("Building indice end %d(%ds)...\n", (int)idxEndTime, (int)(idxEndTime - idxStartTime));
	//dumpStrIndex(1);
	//init_localeinfo (&localeinfo);
	//initialize_unibyte_mask ();

	mainLoop();

	munmap(mmap_addr, st.st_size + 1);

	return 0;
}
