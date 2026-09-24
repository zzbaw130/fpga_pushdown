    /*
     * Small bounded postfix VM.  The instruction set maps to combinational
     * arithmetic/comparison units and shallow mux/boolean pipelines; it is not
     * a general-purpose interpreter and has no jumps or memory allocation.
     */
    bool expression_program(uint32_t index, uint32_t offset, uint32_t length,
                            const CsdSqlExpressionSpec **spec,
                            const CsdSqlExpressionInstruction **instructions)
    {
        if (!blob_range(offset, length) || length < sizeof(CsdSqlExpressionSpec))
            return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        *spec = reinterpret_cast<const CsdSqlExpressionSpec *>(
            static_cast<const unsigned char *>(plan_data_) + offset);
        uint64_t instruction_bytes = 0;
        if ((*spec)->output_count == 0 || (*spec)->instruction_count == 0 ||
            (*spec)->max_stack == 0 || (*spec)->max_stack > 256 ||
            !checked_mul((*spec)->instruction_count, sizeof(CsdSqlExpressionInstruction), &instruction_bytes) ||
            instruction_bytes > length - sizeof(CsdSqlExpressionSpec))
            return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        *instructions = reinterpret_cast<const CsdSqlExpressionInstruction *>(*spec + 1);
        return true;
    }

    static Value boolean_value(bool value)
    {
        Value result;
        result.is_null = false;
        result.type = CSD_SQL_TYPE_BOOL;
        result.integer = value ? 1 : 0;
        return result;
    }

    static bool multiply_power10(__int128 *value, int power)
    {
        if (power < 0 || power > 36) return false;
        const __int128 maximum = static_cast<__int128>((~static_cast<__uint128_t>(0)) >> 1);
        const __int128 minimum = -maximum - 1;
        for (int index = 0; index < power; ++index) {
            if (*value > maximum / 10 || *value < minimum / 10) return false;
            *value *= 10;
        }
        return true;
    }

    static bool scaled_integer(const Value &value, int16_t target_scale, __int128 *result)
    {
		if (target_scale < 0 || target_scale > 36) return false;
        const int16_t source_scale = value.type == CSD_SQL_TYPE_NUMERIC ? value.numeric_scale : 0;
		if (source_scale < 0 || source_scale > 36) return false;
        *result = value.integer;
        if (target_scale >= source_scale)
            return multiply_power10(result, target_scale - source_scale);
        __int128 divisor = 1;
        if (!multiply_power10(&divisor, source_scale - target_scale)) return false;
        *result /= divisor;
        return true;
    }

    static bool fits_i64(__int128 value)
    {
        return value <= std::numeric_limits<int64_t>::max() &&
               value >= std::numeric_limits<int64_t>::min();
    }

	static void numeric_weight_first(const Value &value, int *weight, int *first_digit)
	{
		__int128 magnitude = value.integer < 0 ? -static_cast<__int128>(value.integer) :
			static_cast<__int128>(value.integer);
		const int scale = value.type == CSD_SQL_TYPE_NUMERIC ? value.numeric_scale : 0;
		if (magnitude == 0) {
			*weight = 0;
			*first_digit = 0;
			return;
		}
		int digits = 0;
		for (__int128 cursor = magnitude; cursor != 0; cursor /= 10) ++digits;
		const int decimal_exponent = digits - 1 - scale;
		*weight = decimal_exponent >= 0 ? decimal_exponent / 4 :
			-((-decimal_exponent + 3) / 4);
		const int power = -scale - *weight * 4;
		if (power >= 0)
			multiply_power10(&magnitude, power);
		else {
			__int128 divisor = 1;
			multiply_power10(&divisor, -power);
			magnitude /= divisor;
		}
		*first_digit = static_cast<int>(magnitude);
	}

	/* PostgreSQL numeric.c::select_div_scale, bounded by the software wire's
	 * 128-bit intermediate.  The final Value still has to fit int64. */
	static int16_t division_scale(const Value &left, const Value &right)
	{
		int left_weight, right_weight, left_first, right_first;
		numeric_weight_first(left, &left_weight, &left_first);
		numeric_weight_first(right, &right_weight, &right_first);
		int quotient_weight = left_weight - right_weight;
		if (left_first <= right_first) --quotient_weight;
		const int left_scale = left.type == CSD_SQL_TYPE_NUMERIC ? left.numeric_scale : 0;
		const int right_scale = right.type == CSD_SQL_TYPE_NUMERIC ? right.numeric_scale : 0;
		const int scale = std::max(std::max(16 - quotient_weight * 4, left_scale), right_scale);
		return static_cast<int16_t>(std::max(0, std::min(36, scale)));
	}

    static bool sql_like(const std::string &value, const std::string &pattern, bool insensitive)
    {
        std::vector<uint8_t> previous(pattern.size() + 1, 0), current(pattern.size() + 1, 0);
        previous[0] = 1;
        for (size_t pattern_index = 0; pattern_index < pattern.size() && pattern[pattern_index] == '%'; ++pattern_index)
            previous[pattern_index + 1] = 1;
        for (size_t value_index = 0; value_index < value.size(); ++value_index) {
            current.assign(pattern.size() + 1, 0);
            for (size_t pattern_index = 0; pattern_index < pattern.size(); ++pattern_index) {
                const unsigned char pattern_char = static_cast<unsigned char>(pattern[pattern_index]);
                const unsigned char value_char = static_cast<unsigned char>(value[value_index]);
                if (pattern_char == '%')
                    current[pattern_index + 1] = current[pattern_index] || previous[pattern_index + 1];
                else if (pattern_char == '_' ||
                         (insensitive ? std::tolower(pattern_char) == std::tolower(value_char) : pattern_char == value_char))
                    current[pattern_index + 1] = previous[pattern_index];
            }
            previous.swap(current);
        }
        for (size_t pattern_index = 0; pattern_index < pattern.size(); ++pattern_index)
            if (pattern[pattern_index] == '%') previous[pattern_index + 1] |= previous[pattern_index];
        return previous[pattern.size()] != 0;
    }

    static int32_t postgres_date_year(int64_t postgres_days)
    {
        int64_t z = postgres_days + 10957 + 719468; /* PostgreSQL epoch -> civil epoch. */
        const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        const uint32_t day_of_era = static_cast<uint32_t>(z - era * 146097);
        const uint32_t year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
        int32_t year = static_cast<int32_t>(year_of_era) + static_cast<int32_t>(era * 400);
        const uint32_t day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
        const uint32_t month_prime = (5 * day_of_year + 2) / 153;
        const int32_t month = static_cast<int32_t>(month_prime) + (month_prime < 10 ? 3 : -9);
        year += month <= 2;
        return year;
    }

    bool arithmetic_value(uint32_t index, const CsdSqlExpressionInstruction &instruction,
                          const Value &left, const Value &right, Value *result)
    {
        result->type = instruction.datum_type == CSD_SQL_TYPE_INVALID ?
            static_cast<uint8_t>(CSD_SQL_TYPE_NUMERIC) : instruction.datum_type;
        result->numeric_scale = result->type == CSD_SQL_TYPE_NUMERIC ? instruction.numeric_scale : 0;
		if (result->type == CSD_SQL_TYPE_NUMERIC && result->numeric_scale < 0) {
			const int16_t left_scale = left.type == CSD_SQL_TYPE_NUMERIC ? left.numeric_scale : 0;
			const int16_t right_scale = right.type == CSD_SQL_TYPE_NUMERIC ? right.numeric_scale : 0;
			if (left_scale < 0 || right_scale < 0)
				return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
			if (instruction.opcode == CSD_SQL_EXPR_MULTIPLY)
				result->numeric_scale = std::min<int>(18, left_scale + right_scale);
			else if (instruction.opcode == CSD_SQL_EXPR_DIVIDE)
				result->numeric_scale = division_scale(left, right);
			else
				result->numeric_scale = std::max(left_scale, right_scale);
		}
        if (left.is_null || right.is_null) return true;
        if (left.type == CSD_SQL_TYPE_BYTES || right.type == CSD_SQL_TYPE_BYTES ||
			result->numeric_scale < 0 || result->numeric_scale > 36)
            return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        __int128 value = 0;
        if (instruction.opcode == CSD_SQL_EXPR_ADD || instruction.opcode == CSD_SQL_EXPR_SUBTRACT) {
            __int128 left_value = 0, right_value = 0;
            if (!scaled_integer(left, result->numeric_scale, &left_value) ||
                !scaled_integer(right, result->numeric_scale, &right_value))
                return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
            value = instruction.opcode == CSD_SQL_EXPR_ADD ? left_value + right_value : left_value - right_value;
        } else if (instruction.opcode == CSD_SQL_EXPR_MULTIPLY) {
            value = static_cast<__int128>(left.integer) * right.integer;
            const int source_scale = (left.type == CSD_SQL_TYPE_NUMERIC ? left.numeric_scale : 0) +
                                     (right.type == CSD_SQL_TYPE_NUMERIC ? right.numeric_scale : 0);
            if (result->numeric_scale > source_scale) {
                if (!multiply_power10(&value, result->numeric_scale - source_scale))
                    return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
            } else if (result->numeric_scale < source_scale) {
                __int128 divisor = 1;
                if (!multiply_power10(&divisor, source_scale - result->numeric_scale))
                    return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
                value /= divisor;
            }
		} else {
			if (right.integer == 0) return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
			const int left_scale = left.type == CSD_SQL_TYPE_NUMERIC ? left.numeric_scale : 0;
			const int right_scale = right.type == CSD_SQL_TYPE_NUMERIC ? right.numeric_scale : 0;
			const int minimum_scale = std::max(left_scale, right_scale);
			for (;;) {
				__int128 numerator = left.integer;
				__int128 denominator = right.integer;
				const int exponent = right_scale + result->numeric_scale - left_scale;
				bool scaled = exponent >= 0 ? multiply_power10(&numerator, exponent) :
					multiply_power10(&denominator, -exponent);
				if (scaled) {
					value = numerator / denominator;
					__int128 remainder = numerator % denominator;
					if (remainder < 0) remainder = -remainder;
					__int128 absolute_denominator = denominator < 0 ? -denominator : denominator;
					if (remainder * 2 >= absolute_denominator)
						value += (numerator < 0) == (denominator < 0) ? 1 : -1;
					if (fits_i64(value)) break;
				}
				if (instruction.numeric_scale >= 0 || result->numeric_scale <= minimum_scale)
					return fail(CSD_SQL_STATUS_UNSUPPORTED, index);
				--result->numeric_scale;
			}
        }
        if (!fits_i64(value)) return fail(CSD_SQL_STATUS_UNSUPPORTED, index);
        result->integer = static_cast<int64_t>(value);
        result->is_null = false;
        return true;
    }

    bool evaluate_expression(uint32_t index, uint32_t offset, uint32_t length,
                             const Row &row, const Row *aux_row, std::vector<Value> *outputs)
    {
        const CsdSqlExpressionSpec *spec = NULL;
        const CsdSqlExpressionInstruction *instructions = NULL;
        if (!expression_program(index, offset, length, &spec, &instructions)) return false;
        std::vector<Value> stack;
        stack.reserve(spec->max_stack);
        outputs->clear();
        outputs->reserve(spec->output_count);
        for (uint16_t pc = 0; pc < spec->instruction_count; ++pc) {
            const CsdSqlExpressionInstruction &instruction = instructions[pc];
            if (instruction.opcode < CSD_SQL_EXPR_PUSH_COLUMN || instruction.opcode > CSD_SQL_EXPR_EMIT)
                return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
            if (instruction.opcode == CSD_SQL_EXPR_PUSH_COLUMN || instruction.opcode == CSD_SQL_EXPR_PUSH_AUX_COLUMN) {
                const Row *source = instruction.opcode == CSD_SQL_EXPR_PUSH_COLUMN ? &row : aux_row;
                if (source == NULL || instruction.operand >= source->size())
                    return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
                stack.push_back((*source)[instruction.operand]);
            } else if (instruction.opcode == CSD_SQL_EXPR_PUSH_CONSTANT) {
                if (!is_supported_type(instruction.datum_type))
                    return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
                if (instruction.flags & CSD_SQL_EXPR_PARAMETER_PLACEHOLDER)
                    return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
                Value constant;
                constant.is_null = (instruction.flags & CSD_SQL_EXPR_CONSTANT_NULL) != 0;
                constant.type = instruction.datum_type;
                constant.integer = instruction.immediate;
                constant.numeric_scale = instruction.datum_type == CSD_SQL_TYPE_NUMERIC ? instruction.numeric_scale : 0;
                if (instruction.datum_type == CSD_SQL_TYPE_BYTES && !constant.is_null) {
                    if (!blob_range(instruction.bytes_offset, instruction.bytes_length))
                        return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
                    constant.bytes.assign(static_cast<const char *>(plan_data_) + instruction.bytes_offset,
                                          instruction.bytes_length);
                }
                stack.push_back(constant);
            } else if (instruction.opcode == CSD_SQL_EXPR_EMIT) {
                if (stack.empty() || outputs->size() >= spec->output_count)
                    return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
                outputs->push_back(stack.back());
                stack.pop_back();
            } else if (instruction.opcode == CSD_SQL_EXPR_NOT || instruction.opcode == CSD_SQL_EXPR_IS_NULL ||
                       instruction.opcode == CSD_SQL_EXPR_IS_NOT_NULL || instruction.opcode == CSD_SQL_EXPR_NEGATE ||
                       instruction.opcode == CSD_SQL_EXPR_EXTRACT_YEAR || instruction.opcode == CSD_SQL_EXPR_SUBSTRING) {
                if (stack.empty()) return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
                Value input = stack.back();
                stack.pop_back();
                Value result;
                if (instruction.opcode == CSD_SQL_EXPR_IS_NULL || instruction.opcode == CSD_SQL_EXPR_IS_NOT_NULL) {
                    result = boolean_value(instruction.opcode == CSD_SQL_EXPR_IS_NULL ? input.is_null : !input.is_null);
                } else if (input.is_null) {
                    result.type = instruction.datum_type;
                } else if (instruction.opcode == CSD_SQL_EXPR_NOT) {
                    if (input.type != CSD_SQL_TYPE_BOOL) return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
                    result = boolean_value(input.integer == 0);
                } else if (instruction.opcode == CSD_SQL_EXPR_NEGATE) {
                    if (input.type == CSD_SQL_TYPE_BYTES || input.integer == std::numeric_limits<int64_t>::min())
                        return fail(CSD_SQL_STATUS_UNSUPPORTED, index);
                    result = input;
                    result.integer = -result.integer;
                } else if (instruction.opcode == CSD_SQL_EXPR_EXTRACT_YEAR) {
                    if (input.type != CSD_SQL_TYPE_DATE) return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
                    result.is_null = false;
                    result.type = CSD_SQL_TYPE_INT32;
                    result.integer = postgres_date_year(input.integer);
                } else {
                    if (input.type != CSD_SQL_TYPE_BYTES || instruction.operand == 0)
                        return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
                    const size_t start = instruction.operand - 1;
                    result.is_null = false;
                    result.type = CSD_SQL_TYPE_BYTES;
                    if (start < input.bytes.size()) result.bytes = input.bytes.substr(start, instruction.bytes_length);
                }
                stack.push_back(result);
            } else if (instruction.opcode == CSD_SQL_EXPR_SELECT) {
                if (stack.size() < 3) return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
                Value otherwise = stack.back(); stack.pop_back();
                Value when_true = stack.back(); stack.pop_back();
                Value condition = stack.back(); stack.pop_back();
                if (!condition.is_null && condition.type != CSD_SQL_TYPE_BOOL)
                    return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
                stack.push_back(!condition.is_null && condition.integer != 0 ? when_true : otherwise);
            } else {
                if (stack.size() < 2) return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
                Value right = stack.back(); stack.pop_back();
                Value left = stack.back(); stack.pop_back();
                Value result;
                if (instruction.opcode >= CSD_SQL_EXPR_ADD && instruction.opcode <= CSD_SQL_EXPR_DIVIDE) {
                    if (!arithmetic_value(index, instruction, left, right, &result)) return false;
                } else if (instruction.opcode >= CSD_SQL_EXPR_EQUAL && instruction.opcode <= CSD_SQL_EXPR_GREATER_EQUAL) {
                    result.type = CSD_SQL_TYPE_BOOL;
                    if (!left.is_null && !right.is_null) {
                        const bool numeric_compatible = left.type != CSD_SQL_TYPE_BYTES && right.type != CSD_SQL_TYPE_BYTES &&
                            left.type != CSD_SQL_TYPE_BOOL && right.type != CSD_SQL_TYPE_BOOL;
                        if (left.type != right.type && !numeric_compatible)
                            return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
                        const int comparison = compare_value(left, right);
                        result.is_null = false;
                        switch (instruction.opcode) {
                        case CSD_SQL_EXPR_EQUAL: result.integer = comparison == 0; break;
                        case CSD_SQL_EXPR_NOT_EQUAL: result.integer = comparison != 0; break;
                        case CSD_SQL_EXPR_LESS: result.integer = comparison < 0; break;
                        case CSD_SQL_EXPR_LESS_EQUAL: result.integer = comparison <= 0; break;
                        case CSD_SQL_EXPR_GREATER: result.integer = comparison > 0; break;
                        default: result.integer = comparison >= 0; break;
                        }
                    }
                } else if (instruction.opcode == CSD_SQL_EXPR_AND || instruction.opcode == CSD_SQL_EXPR_OR) {
                    if ((!left.is_null && left.type != CSD_SQL_TYPE_BOOL) ||
                        (!right.is_null && right.type != CSD_SQL_TYPE_BOOL))
                        return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
                    const bool left_true = !left.is_null && left.integer != 0;
                    const bool right_true = !right.is_null && right.integer != 0;
                    if (instruction.opcode == CSD_SQL_EXPR_AND) {
                        if ((!left.is_null && !left_true) || (!right.is_null && !right_true)) result = boolean_value(false);
                        else if (!left.is_null && !right.is_null) result = boolean_value(true);
                        else result.type = CSD_SQL_TYPE_BOOL;
                    } else {
                        if (left_true || right_true) result = boolean_value(true);
                        else if (!left.is_null && !right.is_null) result = boolean_value(false);
                        else result.type = CSD_SQL_TYPE_BOOL;
                    }
                } else if (instruction.opcode == CSD_SQL_EXPR_LIKE) {
                    result.type = CSD_SQL_TYPE_BOOL;
                    if (!left.is_null && !right.is_null) {
                        if (left.type != CSD_SQL_TYPE_BYTES || right.type != CSD_SQL_TYPE_BYTES)
                            return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
                        result = boolean_value(sql_like(left.bytes, right.bytes,
                            (instruction.flags & CSD_SQL_EXPR_CASE_INSENSITIVE) != 0));
                    }
                } else return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
                stack.push_back(result);
            }
            if (stack.size() > spec->max_stack) return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        }
        if (!stack.empty() || outputs->size() != spec->output_count)
            return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        return true;
    }
