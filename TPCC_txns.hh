#pragma once

#include <random>
#include <string>
#include "TPCC_structs.hh"
#include "TPCC_index.hh"

#define NUM_DISTRICTS_PER_WAREHOUSE 10
#define NUM_CUSTOMERS_PER_DISTRICT  3000
#define NUM_ITEMS                   100000

#define A_GEN_CUSTOMER_ID           1023
#define A_GEN_ITEM_ID               8191

#define C_GEN_CUSTOMER_ID           259
#define C_GEN_ITEM_ID               7911

namespace tpcc {

class tpcc_input_generator {
public:
    tpcc_input_generator(int id, int num_whs) : rd(), gen(id), num_whs_(num_whs) {}
    tpcc_input_generator(int num_whs) : rd(), gen(rd()), num_whs_(num_whs) {}

    uint64_t nurand(uint64_t a, uint64_t c, uint64_t x, uint64_t y) {
        uint64_t r1 = random(0, a) | random(x, y) + c;
        return (r1 % (y - x + 1)) + x;
    }
    uint64_t random(uint64_t x, uint64_t y) {
        std::uniform_int_distribution<uint64_t> dist(x, y);
        return dist(gen);
    }
    uint64_t num_warehouses() const {
        return num_whs_;
    }

    uint64_t gen_warehouse_id() {
        return random(1, num_whs_);
    }
    uint64_t gen_customer_id() {
        return nurand(A_GEN_CUSTOMER_ID, C_GEN_CUSTOMER_ID, 1, NUM_CUSTOMERS_PER_DISTRICT);
    }
    uint64_t gen_item_id() {
        return nurand(A_GEN_ITEM_ID, C_GEN_ITEM_ID, 1, NUM_ITEMS);
    }
    uint32_t gen_date() {
        return random(1505244122, 1599938522);
    }

private:
    std::random_device rd;
    std::mt19937 gen;
    uint64_t num_whs_;
};

class tpcc_db {
public:
    typedef unordered_index<warehouse_key, warehouse_value> wh_table_type;
    typedef unordered_index<district_key, district_value>   dt_table_type;
    typedef unordered_index<customer_key, customer_value>   cu_table_type;
    typedef unordered_index<order_key, order_value>         od_table_type;
    typedef unordered_index<orderline_key, orderline_value> ol_table_type;
    typedef unordered_index<order_key, int>                 no_table_type;
    typedef unordered_index<item_key, item_value>           it_table_type;
    typedef unordered_index<stock_key, stock_value>         st_table_type;

    tpcc_db(int num_whs) : num_whs_(num_whs) {}
    tpcc_db(const std::string& db_file_name) {
        (void)db_file_name;
        assert(false);
    }
    ~tpcc_db() {
        delete tbl_whs_;
        delete tbl_dts_;
        delete tbl_cus_;
        delete tbl_ods_;
        delete tbl_ols_;
        delete tbl_nos_;
        delete tbl_its_;
        delete tbl_sts_;
    }

    int num_warehouses() const {
        return num_whs_;
    }
    wh_table_type& tbl_warehouses() {
        return *tbl_whs_;
    }
    dt_table_type& tbl_districts() {
        return *tbl_dts_;
    }
    cu_table_type& tbl_customers() {
        return *tbl_cus_;
    }
    od_table_type& tbl_orders() {
        return *tbl_ods_;
    }
    ol_table_type& tbl_orderlines() {
        return *tbl_ols_;
    }
    no_table_type& tbl_neworders() {
        return *tbl_nos_;
    }
    it_table_type& tbl_items() {
        return *tbl_its_;
    }
    st_table_type& tbl_stocks() {
        return *tbl_sts_;
    }

private:
    int num_whs_;

    wh_table_type *tbl_whs_;
    dt_table_type *tbl_dts_;
    cu_table_type *tbl_cus_;
    od_table_type *tbl_ods_;
    ol_table_type *tbl_ols_;
    no_table_type *tbl_nos_;
    it_table_type *tbl_its_;
    st_table_type *tbl_sts_;
};

class tpcc_runner {
public:
    tpcc_runner(int id, tpcc_db& database, uint64_t w_start, uint64_t w_end)
        : ig(id, database.num_warehouses()), db(database),
          w_id_start(w_start), w_id_end(w_end) {}
    inline void run_txn_neworder();

private:
    tpcc_input_generator ig;
    tpcc_db& db;
    int runner_id;
    uint64_t w_id_start;
    uint64_t w_id_end;
};

class tpcc_prepopulator {
public:
    tpcc_prepopulator(int id, tpcc_db& database)
        : ig(id, database.num_warehouses()), db(database), worker_id(id) {}

    inline void fill_items(uint64_t iid_begin, uint64_t iid_xend);
    inline void fill_warehouses();
    inline void expand_warehouse(uint64_t wid);

private:
    inline std::string random_a_string(int x, int y);
    inline std::string random_n_string(int x, int y);
    inline std::string to_last_name(int gen_n);
    inline std::string random_state_name();
    inline std::string random_zip_code();

    tpcc_input_generator ig;
    tpcc_db& db;
    int worker_id;
};

};
