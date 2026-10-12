function(umd_embed_files OUTPUT)
	set(CONTENT "#include <cstddef>\n\n")
	set(ARGS ${ARGN})
	list(LENGTH ARGS COUNT)
	math(EXPR LAST "${COUNT} - 1")

	foreach(INDEX RANGE 0 ${LAST} 2)
		math(EXPR FILE_INDEX "${INDEX} + 1")
		list(GET ARGS ${INDEX} NAME)
		list(GET ARGS ${FILE_INDEX} FILE)

		file(READ "${FILE}" HEX HEX)
		string(LENGTH "${HEX}" HEX_LENGTH)
		math(EXPR SIZE "${HEX_LENGTH} / 2")
		string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BYTES "${HEX}")

		string(APPEND CONTENT "extern const unsigned char ${NAME}[] = { ${BYTES} };\n")
		string(APPEND CONTENT "extern const size_t ${NAME}Size = ${SIZE};\n\n")
	endforeach()

	if(EXISTS "${OUTPUT}")
		file(READ "${OUTPUT}" EXISTING)
	endif()

	if(NOT "${EXISTING}" STREQUAL "${CONTENT}")
		file(WRITE "${OUTPUT}" "${CONTENT}")
	endif()
endfunction()
