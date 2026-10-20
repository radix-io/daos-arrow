#include <stdexcept>
#include <string>
#include <vector>
#include <daos.h>

namespace daos {

    class exception : public std::runtime_error {
	public:
	    explicit exception(int err_rc, const std::string& msg = "")
		: std::runtime_error(msg + " (" + d_errstr(err_rc) + ")"),
		rc(err_rc) {}
	    const int rc;
    };
    // Helper to check C API return codes and throw on failure
    static inline void check_rc(int rc) {
	if (rc != 0) {
	    throw exception(rc);
	}
    }

    class library {
	public:
	    library() { check_rc(daos_init()); }
	    ~library() { (void) daos_fini(); }

	    // Make the library non-copyable and non-movable as it's a global singleton
	    library(const library&) = delete;
	    library& operator=(const library&) = delete;
	    library(library&&) = delete;
	    library& operator=(library&&) = delete;
    };

    class Container;

    class Pool {
	private:
	    daos_handle_t poh_;
	    // ROOT project defined an event queue in their class

	public:
	    // Constructor connects to the pool: giving no "sys_name" is like passing NULL to the C version
	    Pool(const std::string& pool_label, unsigned int flags) {
		check_rc(daos_pool_connect(pool_label.c_str(), nullptr, flags, &poh_, nullptr, nullptr));
	    }
	    Pool(const std::string& pool_label, const std::string& sys_name, unsigned int flags) {
		check_rc(daos_pool_connect(pool_label.c_str(), sys_name.c_str(), flags, &poh_, nullptr, nullptr));
	    }

	    // Destructor disconnects
	    ~Pool() {
		// Destructors should not throw exceptions
		(void)daos_pool_disconnect(poh_, nullptr);
	    }
	    // Delete copy operations to prevent multiple objects managing the same handle
	    Pool(const Pool&) = delete;
	    Pool& operator=(const Pool&) = delete;

	    // Allow move operations for efficient transfer of ownership (e.g., returning from a function)
	    Pool(Pool&& other) noexcept : poh_(other.poh_) {
		other.poh_.cookie = 0; // Invalidate the moved-from object
	    }
	    Pool& operator=(Pool&& other) noexcept {
		if (this != &other) {
		    if (daos_handle_is_valid(poh_)) {
			(void)daos_pool_disconnect(poh_, nullptr);
		    }
		    poh_ = other.poh_;
		    other.poh_.cookie = 0;
		}
		return *this;
	    }

	    // Member function wrapping daos_cont_create
	    Container create_container(const std::string& label);

	    // Member function wrapping daos_cont_open
	    Container open_container(const std::string& label, unsigned int flags);

	    // Member function wrapping daos_pool_query
	    daos_pool_info_t query() {
		daos_pool_info_t info = {{0}};
		check_rc(daos_pool_query(poh_, nullptr, &info, nullptr, nullptr));
		return info;
	    }

	    // Expose the raw handle for interoperability with the C API if needed
	    daos_handle_t handle() const { return poh_; }
    };

    class Object {
	private:
	    daos_handle_t oh_;

	public:
	    // Note: Constructor is private. Objects are created via Container::open_obj.
	    explicit Object(daos_handle_t oh) : oh_(oh) {}

	    ~Object() {
		if (daos_handle_is_valid(oh_)) {
		    (void)daos_obj_close(oh_, nullptr);
		}
	    }

	    Object(const Object&) = delete;
	    Object& operator=(const Object&) = delete;
	    Object(Object&& other) noexcept : oh_(other.oh_) { other.oh_.cookie = 0; }
	    Object& operator=(Object&& other) noexcept {
		if (this != &other) {
		    if (daos_handle_is_valid(oh_)) {
			(void)daos_obj_close(oh_, nullptr);
		    }
		    oh_ = other.oh_;
		    other.oh_.cookie = 0;
		}
		return *this;
	    }

	    daos_handle_t handle() const { return oh_; }
    };

    class Container {
	private:
	    daos_handle_t coh_;

	public:
	    // Constructor is public, but typically called from Pool::open/create_container
	    explicit Container(daos_handle_t coh) : coh_(coh) {}

	    ~Container() {
		if (daos_handle_is_valid(coh_)) {
		    (void)daos_cont_close(coh_, nullptr);
		}
	    }

	    Container(const Container&) = delete;
	    Container& operator=(const Container&) = delete;
	    Container(Container&& other) noexcept : coh_(other.coh_) { other.coh_.cookie = 0; }
	    Container& operator=(Container&& other) noexcept {
		if (this != &other) {
		    if (daos_handle_is_valid(coh_)) {
			(void)daos_cont_close(coh_, nullptr);
		    }
		    coh_ = other.coh_;
		    other.coh_.cookie = 0;
		}
		return *this;
	    }

	    Object open_obj(daos_obj_id_t oid, unsigned int mode) {
		daos_handle_t oh;
		check_rc(daos_obj_open(coh_, oid, mode, &oh, nullptr));
		return Object(oh);
	    }

	    daos_handle_t handle() const { return coh_; }
    };

    // Implementations for Pool methods that return Container
    inline Container Pool::create_container(const std::string& label) {
	uuid_t uuid;
	daos_prop_t *prop = daos_prop_alloc(1);
	if (prop == NULL) {
	    throw std::bad_alloc();
	}
	prop->dpp_entries[0].dpe_type = DAOS_PROP_CO_LABEL;
	prop->dpp_entries[0].dpe_str = const_cast<char*>(label.c_str());

	int rc = daos_cont_create(poh_, &uuid, prop, nullptr);
	daos_prop_free(prop);
	check_rc(rc);

	// Now open the newly created container
	daos_handle_t coh;
	check_rc(daos_cont_open(poh_, label.c_str(), DAOS_COO_RW, &coh, nullptr, nullptr));
	return Container(coh);
    }

    inline Container Pool::open_container(const std::string& label, unsigned int flags) {
	daos_handle_t coh;
	check_rc(daos_cont_open(poh_, label.c_str(), flags, &coh, nullptr, nullptr));
	return Container(coh);
    }
}

