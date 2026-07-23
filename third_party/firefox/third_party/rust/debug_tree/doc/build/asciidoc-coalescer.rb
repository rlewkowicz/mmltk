#!/usr/bin/env ruby



require 'asciidoctor'
require 'optparse'

options = { attributes: [], output: '-' }
OptionParser.new do |opts|
  opts.banner = 'Usage: ruby asciidoc-coalescer.rb [OPTIONS] FILE'
  opts.on('-a', '--attribute key[=value]', 'A document attribute to set in the form of key[=value]') do |a|
    options[:attributes] << a
  end
  opts.on('-o', '--output FILE', 'Write output to FILE instead of stdout.') do |o|
    options[:output] = o
  end
end.parse!

unless (source_file = ARGV.shift)
  warn 'Please specify an AsciiDoc source file to coalesce.'
  exit 1
end

unless (output_file = options[:output]) == '-'
  if (output_file = File.expand_path output_file) == (File.expand_path source_file)
    warn 'Source and output cannot be the same file.'
    exit 1
  end
end

doc = Asciidoctor.load_file source_file, safe: :unsafe, header_only: true, attributes: options[:attributes]
header_attr_names = (doc.instance_variable_get :@attributes_modified).to_a
header_attr_names.each {|k| doc.attributes[%(#{k}!)] = '' unless doc.attr? k }

doc = Asciidoctor.load_file source_file, safe: :unsafe, parse: false, attributes: doc.attributes
lines = doc.reader.read.gsub(/^include::(?=.*\[\]$)/m, '\\include::')

if output_file == '-'
  puts lines
else
  File.open(output_file, 'w') {|f| f.write lines }
end