#pragma once

#include "helper.h"
#include "stdafx.h"


#define ALIGNED_MALLOC(sz, align) _aligned_malloc(sz, align)

WORKER bstThread(void* _bst);


class ALIGNEDMA
{
public:
	void* operator new(size_t sz);
	void operator delete(void* p);
	
};

// BST things
class Node : public ALIGNEDMA
{
public:
	INT64 volatile key;
	Node* volatile left;
	Node* volatile right;
	Node() { key = 0; right = left = nullptr; }
};

class BST
{
public:
	Node* volatile root;

	BST();
	int contains(INT64 key);
	int add(Node* n);
	Node* remove(INT64 key);
};

void recursiveDelete(Node* n);
bool recursiveVerify(Node* n);