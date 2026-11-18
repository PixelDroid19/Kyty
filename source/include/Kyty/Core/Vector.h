#ifndef INCLUDE_KYTY_CORE_VECTOR_H_
#define INCLUDE_KYTY_CORE_VECTOR_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/SimpleArray.h" // IWYU pragma: export

namespace Kyty::Core {

template <typename T>
class SimpleArray;

template <typename T, class A>
class VectorBase
{
public:
	using DataType        = A;
	using VectorType      = VectorBase<T, A>;
	using SortSwapFunc    = typename A::SortSwapFunc;
	using SortCompareFunc = typename A::SortCompareFunc;

	static constexpr uint32_t INVALID_INDEX = static_cast<uint32_t>(-1);

	// An empty vector owns no storage: draw paths build and drop many empty vectors, and the
	// allocation per construction showed up in their profiles. Storage comes with the first write.
	VectorBase() = default;

	VectorBase(const VectorType& src)
	{
		if (src.m_data != nullptr)
		{
			src.m_data->CopyPtr(&m_data, src.m_data);
		}
	}

	VectorBase(VectorType&& src) noexcept: m_data(src.m_data) { src.m_data = nullptr; }

	explicit VectorBase(uint32_t size, bool ctor = true): m_data(size != 0u ? new DataType(size, ctor) : nullptr) {}

	VectorBase(std::initializer_list<T> list): m_data(new DataType(list)) {}

	virtual ~VectorBase()
	{
		if (m_data)
		{
			m_data->Release();
		}
	}

	[[nodiscard]] uint32_t Size() const { return data()->Size(); }

	[[nodiscard]] uint32_t Capacity() const { return data()->Capacity(); }

	void Clear()
	{
		if (m_data == nullptr)
		{
			return;
		}
		copy_on_write();
		m_data->Clear();
	}

	void Free()
	{
		if (m_data == nullptr)
		{
			return;
		}
		copy_on_write();
		m_data->Free();
	}

	// NOLINTNEXTLINE(bugprone-unhandled-self-assignment,cert-oop54-cpp)
	VectorBase<T, A>& operator=(const VectorType& src)
	{
		if (m_data != src.m_data)
		{
			if (m_data)
			{
				m_data->Release();
			}

			m_data = nullptr;
			if (src.m_data != nullptr)
			{
				src.m_data->CopyPtr(&m_data, src.m_data);
			}
		}
		return *this;
	}

	VectorBase<T, A>& operator=(VectorType&& src) noexcept
	{
		if (m_data != src.m_data)
		{
			if (m_data)
			{
				m_data->Release();
			}

			m_data     = src.m_data;
			src.m_data = nullptr;
		}
		return *this;
	}

	bool operator==(const VectorType& s) const { return *data() == *s.data(); }

	bool operator!=(const VectorType& s) const { return !(*data() == *s.data()); }

	void Expand(uint32_t num)
	{
		copy_on_write();
		m_data->Expand(num);
	}

	void Add(const T& val)
	{
		copy_on_write();
		m_data->Add(val);
	}

	void Add(T&& val)
	{
		copy_on_write();
		m_data->Add(std::forward<T>(val));
	}

	void Add(const T* val, uint32_t num)
	{
		copy_on_write();
		m_data->Add(val, num);
	}

	void Add(const VectorType& v)
	{
		if (this == &v)
		{
			VectorType v2(v);
			copy_on_write();
			m_data->Add(v2.GetData(), v2.Size());
		} else
		{
			copy_on_write();
			m_data->Add(v.GetData(), v.Size());
		}
	}

	void InsertAt(uint32_t index, const T& val)
	{
		copy_on_write();
		m_data->InsertAt(index, val);
	}

	void InsertAt(uint32_t index, T&& val)
	{
		copy_on_write();
		m_data->InsertAt(index, std::forward<T>(val));
	}

	[[nodiscard]] uint32_t Find(const T& t) const { return data()->Find(t); }

	template <typename OP>
	uint32_t Find(const T& t, OP&& op_eq) const
	{
		return data()->Find(t, op_eq);
	}

	template <typename T2, typename OP>
	uint32_t Find(const T2& t, OP&& op_eq) const
	{
		return data()->Find(t, op_eq);
	}

	template <typename T2, typename OP, typename V>
	void FindAll(const T2& t, OP&& op_eq, V* out) const
	{
		data()->FindAll(t, op_eq, out);
	}

	template <typename T2, typename T3, typename OP>
	uint32_t Find(const T2& t2, const T3& t3, OP&& op_eq) const
	{
		return data()->Find(t2, t3, op_eq);
	}

	[[nodiscard]] bool Contains(const T& t) const { return Find(t) != INVALID_INDEX; }

	template <typename T2, typename OP>
	bool Contains(const T2& t, OP&& op_eq) const
	{
		return Find(t, op_eq) != INVALID_INDEX;
	}

	bool Remove(const T& t)
	{
		copy_on_write();
		return m_data->Remove(t);
	}

	bool RemoveAt(uint32_t index, uint32_t count = 1)
	{
		copy_on_write();
		return m_data->RemoveAt(index, count);
	}

	[[nodiscard]] bool IndexValid(uint32_t index) const { return data()->IndexValid(index); }

	T& operator[](uint32_t index)
	{
		copy_on_write();
		return m_data->operator[](index);
	}

	const T& operator[](uint32_t index) const { return data()->At(index); }

	[[nodiscard]] const T& At(uint32_t index) const { return data()->At(index); }

	T* GetData()
	{
		copy_on_write();
		return m_data->GetData();
	}

	[[nodiscard]] const T* GetData() const { return data()->GetDataConst(); }

	[[nodiscard]] const T* GetDataConst() const { return data()->GetDataConst(); }

	void Memset(int c)
	{
		copy_on_write();
		m_data->Memset(c);
	}

	void Sort()
	{
		copy_on_write();
		m_data->Sort();
	}

	void Sort(SortCompareFunc comp_func)
	{
		copy_on_write();
		m_data->Sort(comp_func);
	}

	void Sort(SortSwapFunc swap_func, void* swap_arg = nullptr)
	{
		copy_on_write();
		m_data->Sort(swap_func, swap_arg);
	}

	template <typename OP>
	void Sort(OP&& comp_func)
	{
		copy_on_write();
		m_data->Sort(comp_func);
	}

	[[nodiscard]] bool IsEmpty() const { return Size() == 0; }

	using iterator       = T*;
	using const_iterator = const T*;

	iterator begin() // NOLINT(readability-identifier-naming)
	{
		if (m_data == nullptr)
		{
			return nullptr;
		}
		copy_on_write();
		return m_data->begin();
	}

	iterator end() // NOLINT(readability-identifier-naming)
	{
		if (m_data == nullptr)
		{
			return nullptr;
		}
		copy_on_write();
		return m_data->end();
	}

	[[nodiscard]] const_iterator begin() const { return data()->cbegin(); }  // NOLINT(readability-identifier-naming)
	[[nodiscard]] const_iterator end() const { return data()->cend(); }      // NOLINT(readability-identifier-naming)
	[[nodiscard]] const_iterator cbegin() const { return data()->cbegin(); } // NOLINT(readability-identifier-naming)
	[[nodiscard]] const_iterator cend() const { return data()->cend(); }     // NOLINT(readability-identifier-naming)

private:
	void copy_on_write()
	{
		if (m_data == nullptr)
		{
			m_data = new DataType;
			return;
		}
		m_data->CopyOnWrite(&m_data);
	}

	// Reads of a vector without storage see this shared, never-written empty array.
	[[nodiscard]] const DataType* data() const
	{
		static const DataType empty;
		return m_data != nullptr ? m_data : &empty;
	}

	DataType* m_data = nullptr;
};

} // namespace Kyty::Core

namespace Kyty {

template <typename T>
class Vector: public Core::VectorBase<T, Core::SimpleArray<T>>
{
public:
	using Core::VectorBase<T, Core::SimpleArray<T>>::VectorBase;
};

} // namespace Kyty

#endif /* INCLUDE_KYTY_CORE_VECTOR_H_ */
