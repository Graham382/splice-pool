#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace splicer
{

template<typename T> class Stack;
template<typename T> class SplicePool;

template<typename T>
class Node
{
    friend class SplicePool<T>;
    friend class Stack<T>;

public:
    Node(Node* next = nullptr) : m_val(), m_next(next) { }

    T& val() { return m_val; }
    const T& val() const { return m_val; }

private:
    Node* next() { return m_next; }
    void next(Node* node) { m_next = node; }

    T m_val;
    Node* m_next;
};

template<typename T>
class Stack
{
    friend class SplicePool<T>;

public:
    Stack() : m_tail(nullptr), m_head(nullptr) { }

    void push(Node<T>* node)
    {
        node->next(m_head);
        m_head = node;

        if (!m_tail) m_tail = node;
    }

    void push(Stack& other)
    {
        if (!other.empty())
        {
            push(other.m_tail);
            m_head = other.m_head;
        }
    }

    Node<T>* pop()
    {
        Node<T>* node(m_head);
        if (m_head)
        {
            m_head = m_head->next();
            if (!m_head) m_tail = nullptr;
        }
        return node;
    }

    bool empty() const { return !m_head; }

    void print(std::size_t maxElements = 10) const
    {
        if (Node<T>* current = m_head)
        {
            std::size_t i(0);

            while (current && i++ < maxElements)
            {
                std::cout << current->val() << " ";
                current = current->next();
            }

            if (current) std::cout << "and more..." << std::endl;

            std::cout << std::endl;
        }
        else
        {
            std::cout << "(empty)" << std::endl;
        }
    }

private:
    Node<T>* head() { return m_head; }

    Node<T>* m_tail;
    Node<T>* m_head;
};

class TryLocker
{
public:
    explicit TryLocker(std::atomic_flag& flag)
        : m_set(false)
        , m_flag(flag)
    { }

    ~TryLocker()
    {
        if (m_set) m_flag.clear();
    }

    bool tryLock()
    {
        m_set = !m_flag.test_and_set();
        return m_set;
    }

private:
    bool m_set;
    std::atomic_flag& m_flag;
};

template<typename T>
class SplicePool
{
public:
    typedef Node<T> NodeType;
    typedef Stack<T> StackType;

    SplicePool(std::size_t blockSize)
        : m_stack()
        , m_blockSize(blockSize)
        , m_mutex()
        , m_adding()
        , m_count(0)
    {
        m_adding.clear();
    }

    virtual ~SplicePool() { }

    std::size_t count() const { return m_count.load(); }

    void release(Node<T>* node)
    {
        reset(&node->val());

        std::lock_guard<std::mutex> lock(m_mutex);
        m_stack.push(node);
    }

    void release(Stack<T>& other)
    {
        Node<T>* node(other.head());
        while (node)
        {
            reset(&node->val());
            node = node->next();
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        m_stack.push(other);
    }

    template<class... Args>
    Node<T>* acquire(Args&&... args)
    {
        Node<T>* node(0);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            node = m_stack.pop();
        }

        if (node)
        {
            if (!std::is_pointer<T>::value)
            {
                new (&node->val()) T(std::forward<Args>(args)...);
            }

            return node;
        }
        else
        {
            allocate();
            return acquire(std::forward<Args>(args)...);
        }
    }

    void print(std::size_t maxElements) const { m_stack.print(maxElements); }

protected:
    void allocate()
    {
        TryLocker locker(m_adding);

        if (locker.tryLock())
        {
            doAllocate();
            m_count += m_blockSize;
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void reset(T* val)
    {
        destruct(val);
        construct(val);
    }

    virtual void doAllocate() = 0;
    virtual void construct(T*) { }
    virtual void destruct(T*) { }

    Stack<T> m_stack;
    const std::size_t m_blockSize;
    std::mutex m_mutex;

private:
    std::atomic_flag m_adding;
    std::atomic_size_t m_count;
};

template<typename T>
class ObjectPool : public SplicePool<T>
{
public:
    ObjectPool(std::size_t blockSize = 4096)
        : SplicePool<T>(blockSize)
        , m_blocks()
    { }

private:
    virtual void doAllocate()
    {
        {
            std::unique_ptr<std::vector<Node<T>>> newBlock(
                    new std::vector<Node<T>>(this->m_blockSize));
            m_blocks.push_back(std::move(newBlock));
        }

        std::vector<Node<T>>& newBlock(*m_blocks.back());
        Stack<T> newStack;

        for (std::size_t i(0); i < newBlock.size(); ++i)
        {
            newStack.push(&newBlock[i]);
        }

        std::lock_guard<std::mutex> lock(this->m_mutex);
        this->m_stack.push(newStack);
    }

    virtual void construct(T* val)
    {
        new (val) T();
    }

    virtual void destruct(T* val)
    {
        val->~T();
    }

    std::deque<std::unique_ptr<std::vector<Node<T>>>> m_blocks;
};

template<typename T>
class BufferPool : public SplicePool<T*>
{
public:
    BufferPool(std::size_t bufferSize, std::size_t blockSize = 1)
        : SplicePool<T*>(blockSize)
        , m_bufferSize(bufferSize)
        , m_bytesPerBlock(m_bufferSize * this->m_blockSize)
        , m_bytes()
        , m_nodes()
    { }

private:
    virtual void doAllocate()
    {
        {
            std::unique_ptr<std::vector<T>> newBytes(
                    new std::vector<T>(m_bytesPerBlock));
            m_bytes.push_back(std::move(newBytes));
        }

        {
            std::unique_ptr<std::vector<Node<T*>>> newNodes(
                    new std::vector<Node<T*>>(this->m_blockSize));
            m_nodes.push_back(std::move(newNodes));
        }

        std::vector<T>& newBytes(*m_bytes.back());
        std::vector<Node<T*>>& newNodes(*m_nodes.back());
        Stack<T*> newStack;

        for (std::size_t i(0); i < this->m_blockSize; ++i)
        {
            Node<T*>& node(newNodes[i]);
            node.val() = &newBytes[m_bufferSize * i];
            newStack.push(&node);
        }

        std::lock_guard<std::mutex> lock(this->m_mutex);
        this->m_stack.push(newStack);
    }

    virtual void construct(T** val)
    {
        std::fill(*val, *val + m_bufferSize, 0);
    }

    const std::size_t m_bufferSize;
    const std::size_t m_bytesPerBlock;

    std::deque<std::unique_ptr<std::vector<T>>> m_bytes;
    std::deque<std::unique_ptr<std::vector<Node<T*>>>> m_nodes;
};

} // namespace splicer

