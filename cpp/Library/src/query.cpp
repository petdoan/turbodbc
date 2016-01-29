#include <turbodbc/query.h>
#include <turbodbc/make_description.h>
#include <turbodbc/descriptions/integer_description.h>

#include <cpp_odbc/error.h>

#include <boost/variant/get.hpp>
#include <sqlext.h>
#include <stdexcept>

#include <cstring>
#include <sstream>


namespace turbodbc {

namespace {

	std::shared_ptr<parameter> make_parameter(cpp_odbc::statement const & statement, std::size_t one_based_index, std::size_t buffered_sets)
	{
		auto const description = statement.describe_parameter(one_based_index);
		return std::make_shared<parameter>(statement, one_based_index, buffered_sets, make_description(description));
	}

}

query::query(std::shared_ptr<cpp_odbc::statement const> statement, std::size_t rows_to_buffer, std::size_t parameter_sets_to_buffer) :
	statement_(statement),
	rows_to_buffer_(rows_to_buffer),
	parameter_sets_to_buffer_(parameter_sets_to_buffer),
	current_parameter_set_(0),
	row_count_(0),
	rows_processed_(0)
{
	bind_parameters();
}

query::~query()
{
	statement_->close_cursor();
}

void query::execute()
{
	execute_batch();

	std::size_t const columns = statement_->number_of_columns();
	if (columns != 0) {
		result_ = std::make_shared<result_set>(statement_, rows_to_buffer_);
	}
}

void query::add_parameter_set(std::vector<nullable_field> const & parameter_set)
{
	check_parameter_set(parameter_set);

	if (current_parameter_set_ == parameter_sets_to_buffer_) {
		execute_batch();
	}

	for (unsigned int p = 0; p != parameter_set.size(); ++p) {
		add_parameter(p, parameter_set[p]);
	}

	++current_parameter_set_;
}

std::vector<nullable_field> query::fetch_one()
{
	if (result_) {
		return result_->fetch_one();
	} else {
		throw std::runtime_error("No active result set");
	}
}

long query::get_row_count()
{
	return row_count_;
}

std::vector<column_info> query::get_result_set_info() const
{
	if (result_) {
		return result_->get_info();
	} else {
		return {};
	}
}

std::size_t query::execute_batch()
{
	std::size_t result = 0;

	if (not parameters_.empty()) {
		statement_->set_attribute(SQL_ATTR_PARAMSET_SIZE, current_parameter_set_);
	}

	if ((current_parameter_set_ != 0) or parameters_.empty()){
		statement_->execute_prepared();
		update_row_count();
		result = parameters_.empty() ? 1 : current_parameter_set_;
	}

	current_parameter_set_ = 0;
	return result;
}

void query::bind_parameters()
{
	if (statement_->number_of_parameters() != 0) {
		std::size_t const n_parameters = statement_->number_of_parameters();
		for (std::size_t one_based_index = 1; one_based_index <= n_parameters; ++one_based_index) {
			parameters_.push_back(make_parameter(*statement_, one_based_index, parameter_sets_to_buffer_));
		}
		statement_->set_attribute(SQL_ATTR_PARAMSET_SIZE, current_parameter_set_);
		statement_->set_attribute(SQL_ATTR_PARAMS_PROCESSED_PTR, &rows_processed_);
	}
}

void query::check_parameter_set(std::vector<nullable_field> const & parameter_set) const
{
	if (parameter_set.size() != parameters_.size()) {
		std::ostringstream message;
		message << "Invalid number of parameters (expected " << parameters_.size()
				<< ", got " << parameter_set.size() << ")";
		throw cpp_odbc::error(message.str());
	}
}

void query::add_parameter(std::size_t index, nullable_field const & value)
{
	try {
		parameters_[index]->set(current_parameter_set_, value);
	} catch (boost::bad_get const &) {
		auto const last_active_row = execute_batch();
		recover_unwritten_parameters_below(index, last_active_row);
		rebind_parameter_to_hold_value(index, *value);
		parameters_[index]->set(current_parameter_set_, value);
	}
}

void query::recover_unwritten_parameters_below(std::size_t parameter_index, std::size_t last_active_row)
{
	for (std::size_t i = 0; i != parameter_index; ++i) {
		parameters_[i]->copy_to_first_row(last_active_row);
	}
}

void query::rebind_parameter_to_hold_value(std::size_t index, field const & value)
{
	auto description = make_description(value);
	parameters_[index] = std::make_shared<parameter>(*statement_, index + 1, parameter_sets_to_buffer_, std::move(description));
}

void query::update_row_count()
{
	bool const has_result_set = (statement_->number_of_columns() != 0);
	if (has_result_set) {
		row_count_ = statement_->row_count();
	} else {
		row_count_ += rows_processed_;
	}
}

}
