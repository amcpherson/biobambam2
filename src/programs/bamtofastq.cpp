/**
    biobambam
    Copyright (C) 2009-2013 German Tischler
    Copyright (C) 2011-2013 Genome Research Limited

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
**/

#include <biobambam/BamBamConfig.hpp>
#include <biobambam/Licensing.hpp>

#include <iomanip>

#include <config.h>

#include <libmaus/bambam/CircularHashCollatingBamDecoder.hpp>
#include <libmaus/bambam/BamToFastqOutputFileSet.hpp>
#include <libmaus/util/TempFileRemovalContainer.hpp>
#include <libmaus/util/MemUsage.hpp>

static std::string getDefaultInputFormat()
{
	return "bam";
}

struct BamToFastQInputFileStream
{
	std::string const fn;
	libmaus::aio::CheckedInputStream::unique_ptr_type CIS;
	std::istream & in;
	
	static libmaus::aio::CheckedInputStream::unique_ptr_type openFile(std::string const & fn)
	{
		libmaus::aio::CheckedInputStream::unique_ptr_type ptr(new libmaus::aio::CheckedInputStream(fn));
		return UNIQUE_PTR_MOVE(ptr);
	}
	
	BamToFastQInputFileStream(libmaus::util::ArgInfo const & arginfo)
	: fn(arginfo.getValue<std::string>("filename","-")),
	  CIS(
		(fn != "-") ? openFile(fn) : libmaus::aio::CheckedInputStream::unique_ptr_type()
	  ), in((fn != "-") ? (*CIS) : std::cin) {}

	BamToFastQInputFileStream(std::string const & rfn)
	: fn(rfn), CIS(
		(fn != "-") ? openFile(fn) : libmaus::aio::CheckedInputStream::unique_ptr_type()
	), in((fn != "-") ? (*CIS) : std::cin) {}
};

void bamtofastqNonCollating(libmaus::util::ArgInfo const & arginfo, libmaus::bambam::BamAlignmentDecoder & bamdec)
{
	if ( arginfo.getValue<unsigned int>("disablevalidation",0) )
		bamdec.disableValidation();

	libmaus::timing::RealTimeClock rtc; rtc.start();
	uint32_t const excludeflags = libmaus::bambam::BamFlagBase::stringToFlags(arginfo.getValue<std::string>("exclude","SECONDARY,SUPPLEMENTARY"));
	libmaus::bambam::BamAlignment const & algn = bamdec.getAlignment();
	::libmaus::autoarray::AutoArray<uint8_t> T;
	uint64_t cnt = 0;
	uint64_t bcnt = 0;
	unsigned int const verbshift = 20;
		
	while ( bamdec.readAlignment() )
	{
		uint64_t const precnt = cnt++;
		
		if ( ! (algn.getFlags() & excludeflags) )
		{
			uint64_t la = libmaus::bambam::BamAlignmentDecoderBase::putFastQ(algn.D.begin(),T);
			std::cout.write(reinterpret_cast<char const *>(T.begin()),la);
			bcnt += la;
		}

		if ( precnt >> verbshift != cnt >> verbshift )
		{
			std::cerr 
				<< (cnt >> 20) 
				<< "\t"
				<< (static_cast<double>(bcnt)/(1024.0*1024.0))/rtc.getElapsedSeconds() << "MB/s"
				<< "\t" << static_cast<double>(cnt)/rtc.getElapsedSeconds() << std::endl;
		}
	}

}

void bamtofastqNonCollating(libmaus::util::ArgInfo const & arginfo)
{
	std::string const inputformat = arginfo.getValue<std::string>("inputformat",getDefaultInputFormat());
	std::string const inputfilename = arginfo.getValue<std::string>("filename","-");
	uint64_t const numthreads = arginfo.getValue<uint64_t>("threads",0);
	
	if ( inputformat == "bam" )
	{
		if ( arginfo.hasArg("ranges") )
		{
			libmaus::bambam::BamRangeDecoder bamdec(inputfilename,arginfo.getUnparsedValue("ranges",""));
			bamtofastqNonCollating(arginfo,bamdec);
		}
		else
		{
			BamToFastQInputFileStream bamin(inputfilename);
			if ( numthreads > 0 )
			{
				libmaus::bambam::BamParallelDecoderWrapper bamdecwrap(bamin.in,numthreads);
				bamtofastqNonCollating(arginfo,bamdecwrap.getDecoder());	
			}
			else
			{
				libmaus::bambam::BamDecoder bamdec(bamin.in);
				bamtofastqNonCollating(arginfo,bamdec);
			}
		}
	}
	#if defined(BIOBAMBAM_LIBMAUS_HAVE_IO_LIB)
	else if ( inputformat == "sam" )
	{
		libmaus::bambam::ScramDecoder bamdec(inputfilename,"r","");
		bamtofastqNonCollating(arginfo,bamdec);
	}
	else if ( inputformat == "cram" )
	{
		std::string const reference = arginfo.getValue<std::string>("reference","");
		libmaus::bambam::ScramDecoder bamdec(inputfilename,"rc",reference);
		bamtofastqNonCollating(arginfo,bamdec);
	}
	#endif
	else
	{
		libmaus::exception::LibMausException se;
		se.getStream() << "unknown input format " << inputformat << std::endl;
		se.finish();
		throw se;
	}
		
	std::cout.flush();
}

struct CollateCombs
{
	uint64_t pairs;
	uint64_t orphans1;
	uint64_t orphans2;
	uint64_t single;
	uint64_t alignments;

	CollateCombs()
	: pairs(0), orphans1(0), orphans2(0), single(0), alignments(0)
	{
	
	}
};

std::ostream & operator<<(std::ostream & out, CollateCombs const & combs)
{
	out << "[C]\tAlignments:\t" << combs.alignments << std::endl;
	out << "[C]\tComplete pairs:\t" << combs.pairs << std::endl;
	out << "[C]\tSingle:\t" << combs.single << std::endl;
	out << "[C]\tOrphans:\t" << (combs.orphans1 + combs.orphans2) 
		<< "\t" << combs.orphans1 << "\t" << combs.orphans2 << std::endl;
	return out;
};

void bamtofastqCollating(
	libmaus::util::ArgInfo const & arginfo,
	libmaus::bambam::CircularHashCollatingBamDecoder & CHCBD
)
{
	if ( arginfo.getValue<unsigned int>("disablevalidation",0) )
		CHCBD.disableValidation();

	libmaus::bambam::BamToFastqOutputFileSet OFS(arginfo);

	libmaus::bambam::CircularHashCollatingBamDecoder::OutputBufferEntry const * ob = 0;
	
	// number of alignments written to files
	uint64_t cnt = 0;
	// number of bytes written to files
	uint64_t bcnt = 0;
	unsigned int const verbshift = 20;
	libmaus::timing::RealTimeClock rtc; rtc.start();
	::libmaus::autoarray::AutoArray<uint8_t> T;
	CollateCombs combs;
		
	while ( (ob = CHCBD.process()) )
	{
		uint64_t const precnt = cnt;
		
		if ( ob->fpair )
		{
			uint64_t la = libmaus::bambam::BamAlignmentDecoderBase::putFastQ(ob->Da,T);
			OFS.Fout.write(reinterpret_cast<char const *>(T.begin()),la);
			uint64_t lb = libmaus::bambam::BamAlignmentDecoderBase::putFastQ(ob->Db,T);
			OFS.F2out.write(reinterpret_cast<char const *>(T.begin()),lb);

			combs.pairs += 1;
			cnt += 2;
			bcnt += (la+lb);
		}
		else if ( ob->fsingle )
		{
			uint64_t la = libmaus::bambam::BamAlignmentDecoderBase::putFastQ(ob->Da,T);
			OFS.Sout.write(reinterpret_cast<char const *>(T.begin()),la);

			combs.single += 1;
			cnt += 1;
			bcnt += (la);
		}
		else if ( ob->forphan1 )
		{
			uint64_t la = libmaus::bambam::BamAlignmentDecoderBase::putFastQ(ob->Da,T);
			OFS.Oout.write(reinterpret_cast<char const *>(T.begin()),la);

			combs.orphans1 += 1;
			cnt += 1;
			bcnt += (la);
		}
		else if ( ob->forphan2 )
		{
			uint64_t la = libmaus::bambam::BamAlignmentDecoderBase::putFastQ(ob->Da,T);
			OFS.O2out.write(reinterpret_cast<char const *>(T.begin()),la);

			combs.orphans2 += 1;
			cnt += 1;
			bcnt += (la);
		}
		
		if ( precnt >> verbshift != cnt >> verbshift )
		{
			std::cerr 
				<< "[V] "
				<< (cnt >> 20) 
				<< "\t"
				<< (static_cast<double>(bcnt)/(1024.0*1024.0))/rtc.getElapsedSeconds() << "MB/s"
				<< "\t" << static_cast<double>(cnt)/rtc.getElapsedSeconds() << std::endl;
		}
	}

	std::cerr << "[V] " << cnt << std::endl;
	
	combs.alignments = CHCBD.getRank();

	if ( arginfo.getValue<unsigned int>("combs",0) )
		std::cerr << combs;
}

void bamtofastqCollating(libmaus::util::ArgInfo const & arginfo)
{
	uint32_t const excludeflags = libmaus::bambam::BamFlagBase::stringToFlags(arginfo.getValue<std::string>("exclude","SECONDARY,SUPPLEMENTARY"));
	libmaus::util::TempFileRemovalContainer::setup();
	std::string const tmpfilename = arginfo.getValue<std::string>("T",arginfo.getDefaultTmpFileName());
	libmaus::util::TempFileRemovalContainer::addTempFile(tmpfilename);
	std::string const inputformat = arginfo.getValue<std::string>("inputformat",getDefaultInputFormat());
	std::string const inputfilename = arginfo.getValue<std::string>("filename","-");
	uint64_t const numthreads = arginfo.getValue<uint64_t>("threads",0);

	unsigned int const hlog = arginfo.getValue<unsigned int>("colhlog",18);
	uint64_t const sbs = arginfo.getValueUnsignedNumeric<uint64_t>("colsbs",128ull*1024ull*1024ull);

	if ( inputformat == "bam" )
	{
		if ( arginfo.hasArg("ranges") )
		{
			libmaus::bambam::BamRangeCircularHashCollatingBamDecoder CHCBD(inputfilename,arginfo.getUnparsedValue("ranges",""),tmpfilename,excludeflags,false,hlog,sbs);
			bamtofastqCollating(arginfo,CHCBD);
		}
		else
		{
			BamToFastQInputFileStream bamin(inputfilename);

			if ( numthreads > 0 )
			{
				libmaus::bambam::BamParallelCircularHashCollatingBamDecoder CHCBD(
					bamin.in,
					numthreads,
					tmpfilename,excludeflags,
					false, /* put rank */
					hlog,
					sbs
					);
				bamtofastqCollating(arginfo,CHCBD);
			}
			else
			{
				libmaus::bambam::BamCircularHashCollatingBamDecoder CHCBD(
					bamin.in,
					tmpfilename,excludeflags,
					false, /* put rank */
					hlog,
					sbs
					);
				bamtofastqCollating(arginfo,CHCBD);
			}
		}
	}
	#if defined(BIOBAMBAM_LIBMAUS_HAVE_IO_LIB)
	else if ( inputformat == "sam" )
	{
		libmaus::bambam::ScramCircularHashCollatingBamDecoder CHCBD(inputfilename,"r","",
			tmpfilename,excludeflags,
			false, /* put rank */
			hlog,sbs
		);
		bamtofastqCollating(arginfo,CHCBD);
	}
	else if ( inputformat == "cram" )
	{
		std::string const reference = arginfo.getValue<std::string>("reference","");
		libmaus::bambam::ScramCircularHashCollatingBamDecoder CHCBD(inputfilename,"rc",reference,
			tmpfilename,excludeflags,
			false, /* put rank */
			hlog,sbs
		);
		bamtofastqCollating(arginfo,CHCBD);
	}
	#endif
	else
	{
		libmaus::exception::LibMausException se;
		se.getStream() << "unknown input format " << inputformat << std::endl;
		se.finish();
		throw se;
	}
	
	std::cout.flush();
}

void bamtofastqCollatingRanking(
	libmaus::util::ArgInfo const & arginfo,
	libmaus::bambam::CircularHashCollatingBamDecoder & CHCBD
)
{
	if ( arginfo.getValue<unsigned int>("disablevalidation",0) )
		CHCBD.disableValidation();

	libmaus::bambam::BamToFastqOutputFileSet OFS(arginfo);

	libmaus::bambam::CircularHashCollatingBamDecoder::OutputBufferEntry const * ob = 0;
	
	// number of alignments written to files
	uint64_t cnt = 0;
	// number of bytes written to files
	uint64_t bcnt = 0;
	unsigned int const verbshift = 20;
	libmaus::timing::RealTimeClock rtc; rtc.start();
	::libmaus::autoarray::AutoArray<uint8_t> T;
	CollateCombs combs;
	
	while ( (ob = CHCBD.process()) )
	{
		uint64_t const precnt = cnt;
		
		if ( ob->fpair )
		{
			uint64_t const ranka = libmaus::bambam::BamAlignmentDecoderBase::getRank(ob->Da,ob->blocksizea);
			uint64_t const rankb = libmaus::bambam::BamAlignmentDecoderBase::getRank(ob->Db,ob->blocksizeb);
			uint64_t const la = libmaus::bambam::BamAlignmentDecoderBase::putFastQRanks(ob->Da,ranka,rankb,T);		
			OFS.Fout.write(reinterpret_cast<char const *>(T.begin()),la);
			uint64_t lb = libmaus::bambam::BamAlignmentDecoderBase::putFastQRanks(ob->Db,ranka,rankb,T);
			OFS.F2out.write(reinterpret_cast<char const *>(T.begin()),lb);

			combs.pairs += 1;
			cnt += 2;
			bcnt += (la+lb);
		}
		else if ( ob->fsingle )
		{
			uint64_t const ranka = libmaus::bambam::BamAlignmentDecoderBase::getRank(ob->Da,ob->blocksizea);
			
			uint64_t la = libmaus::bambam::BamAlignmentDecoderBase::putFastQRanks(ob->Da,ranka,ranka,T);
			OFS.Sout.write(reinterpret_cast<char const *>(T.begin()),la);

			combs.single += 1;
			cnt += 1;
			bcnt += (la);
		}
		else if ( ob->forphan1 )
		{
			uint64_t const ranka = libmaus::bambam::BamAlignmentDecoderBase::getRank(ob->Da,ob->blocksizea);
			
			uint64_t la = libmaus::bambam::BamAlignmentDecoderBase::putFastQRanks(ob->Da,ranka,ranka,T);
			OFS.Oout.write(reinterpret_cast<char const *>(T.begin()),la);

			combs.orphans1 += 1;
			cnt += 1;
			bcnt += (la);
		}
		else if ( ob->forphan2 )
		{
			uint64_t const ranka = libmaus::bambam::BamAlignmentDecoderBase::getRank(ob->Da,ob->blocksizea);
			
			uint64_t la = libmaus::bambam::BamAlignmentDecoderBase::putFastQRanks(ob->Da,ranka,ranka,T);
			OFS.O2out.write(reinterpret_cast<char const *>(T.begin()),la);

			combs.orphans2 += 1;
			cnt += 1;
			bcnt += (la);
		}
		
		if ( precnt >> verbshift != cnt >> verbshift )
		{
			std::cerr 
				<< "[V] "
				<< (cnt >> 20) 
				<< "\t"
				<< (static_cast<double>(bcnt)/(1024.0*1024.0))/rtc.getElapsedSeconds() << "MB/s"
				<< "\t" << static_cast<double>(cnt)/rtc.getElapsedSeconds() << std::endl;
		}
	}
	
	std::cerr << "[V] " << cnt << std::endl;

	combs.alignments = CHCBD.getRank();

	if ( arginfo.getValue<unsigned int>("combs",0) )
		std::cerr << combs;
}

void bamtofastqCollatingRanking(libmaus::util::ArgInfo const & arginfo)
{
	uint32_t const excludeflags = libmaus::bambam::BamFlagBase::stringToFlags(arginfo.getValue<std::string>("exclude","SECONDARY,SUPPLEMENTARY"));
	libmaus::util::TempFileRemovalContainer::setup();
	std::string const tmpfilename = arginfo.getValue<std::string>("T",arginfo.getDefaultTmpFileName());
	libmaus::util::TempFileRemovalContainer::addTempFile(tmpfilename);
	std::string const inputformat = arginfo.getValue<std::string>("inputformat",getDefaultInputFormat());
	std::string const inputfilename = arginfo.getValue<std::string>("filename","-");
	uint64_t const numthreads = arginfo.getValue<uint64_t>("threads",0);

	unsigned int const hlog = arginfo.getValue<unsigned int>("colhlog",18);
	uint64_t const sbs = arginfo.getValueUnsignedNumeric<uint64_t>("colsbs",128ull*1024ull*1024ull);

	if ( inputformat == "bam" )
	{
		BamToFastQInputFileStream bamin(inputfilename);

		if ( numthreads > 0 )
		{
			libmaus::bambam::BamParallelCircularHashCollatingBamDecoder CHCBD(
				bamin.in,
				numthreads,
				tmpfilename,excludeflags,
				true, /* put rank */
				hlog,
				sbs
				);
			bamtofastqCollatingRanking(arginfo,CHCBD);
		}
		else
		{
			libmaus::bambam::BamCircularHashCollatingBamDecoder CHCBD(
				bamin.in,
				tmpfilename,excludeflags,
				true, /* put rank */
				hlog,
				sbs
				);
			bamtofastqCollatingRanking(arginfo,CHCBD);
		}
	}
	#if defined(BIOBAMBAM_LIBMAUS_HAVE_IO_LIB)
	else if ( inputformat == "sam" )
	{
		libmaus::bambam::ScramCircularHashCollatingBamDecoder CHCBD(inputfilename,"r","",
			tmpfilename,excludeflags,
			true, /* put rank */
			hlog,sbs
		);
		bamtofastqCollatingRanking(arginfo,CHCBD);
	}
	else if ( inputformat == "cram" )
	{
		std::string const reference = arginfo.getValue<std::string>("reference","");
		libmaus::bambam::ScramCircularHashCollatingBamDecoder CHCBD(inputfilename,"rc",reference,
			tmpfilename,excludeflags,
			true, /* put rank */
			hlog,sbs
		);
		bamtofastqCollatingRanking(arginfo,CHCBD);
	}
	#endif
	else
	{
		libmaus::exception::LibMausException se;
		se.getStream() << "unknown input format " << inputformat << std::endl;
		se.finish();
		throw se;
	}
	
	std::cout.flush();
}

void bamtofastq(libmaus::util::ArgInfo const & arginfo)
{
	if ( arginfo.hasArg("ranges") && arginfo.getValue("inputformat", getDefaultInputFormat()) != "bam" )
	{
		libmaus::exception::LibMausException se;
		se.getStream() << "ranges are only supported for inputformat=bam" << std::endl;
		se.finish();
		throw se;
	}

	if ( arginfo.hasArg("ranges") && ((!arginfo.hasArg("filename")) || arginfo.getValue<std::string>("filename","-") == "-") )
	{
		libmaus::exception::LibMausException se;
		se.getStream() << "ranges are not supported for reading via standard input" << std::endl;
		se.finish();
		throw se;
	}

	if ( arginfo.hasArg("ranges") && arginfo.getValue<uint64_t>("collate",1) > 1 )
	{
		libmaus::exception::LibMausException se;
		se.getStream() << "ranges are not supported for collate > 1" << std::endl;
		se.finish();
		throw se;
	}

	switch ( arginfo.getValue<uint64_t>("collate",1) )
	{
		case 0:
			bamtofastqNonCollating(arginfo);
			break;
		case 1:
			bamtofastqCollating(arginfo);
			break;
		case 2:		
			bamtofastqCollatingRanking(arginfo);
			break;
		default:
		{
			libmaus::exception::LibMausException se;
			se.getStream() << "unknown collate argument " << arginfo.getValue<uint64_t>("collate",1) << std::endl;
			se.finish();
			throw se;
		}
	}
}

int main(int argc, char * argv[])
{
	try
	{
		libmaus::timing::RealTimeClock rtc; rtc.start();
		
		::libmaus::util::ArgInfo const arginfo(argc,argv);
		
		for ( uint64_t i = 0; i < arginfo.restargs.size(); ++i )
			if ( 
				arginfo.restargs[i] == "-v"
				||
				arginfo.restargs[i] == "--version"
			)
			{
				std::cerr << ::biobambam::Licensing::license();
				return EXIT_SUCCESS;
			}
			else if ( 
				arginfo.restargs[i] == "-h"
				||
				arginfo.restargs[i] == "--help"
			)
			{
				std::cerr << ::biobambam::Licensing::license() << std::endl;
				std::cerr << "Key=Value pairs:" << std::endl;
				std::cerr << std::endl;
				
				std::vector< std::pair<std::string,std::string> > V;
				
				V.push_back ( std::pair<std::string,std::string> ( "F=<[stdout]>", "matched pairs first mates" ) );
				V.push_back ( std::pair<std::string,std::string> ( "F2=<[stdout]>", "matched pairs second mates" ) );
				V.push_back ( std::pair<std::string,std::string> ( "S=<[stdout]>", "single end" ) );
				V.push_back ( std::pair<std::string,std::string> ( "O=<[stdout]>", "unmatched pairs first mates" ) );
				V.push_back ( std::pair<std::string,std::string> ( "O2=<[stdout]>", "unmatched pairs second mates" ) );
				V.push_back ( std::pair<std::string,std::string> ( "collate=<[1]>", "collate pairs" ) );
				V.push_back ( std::pair<std::string,std::string> ( "combs=<[0]>", "print some counts after collation based processing" ) );
				V.push_back ( std::pair<std::string,std::string> ( "filename=<[stdin]>", "input filename (default: read file from standard input)" ) );
				#if defined(BIOBAMBAM_LIBMAUS_HAVE_IO_LIB)
				V.push_back ( std::pair<std::string,std::string> ( std::string("inputformat=<[")+getDefaultInputFormat()+"]>", "input format: cram, bam or sam" ) );
				V.push_back ( std::pair<std::string,std::string> ( "reference=<[]>", "name of reference FastA in case of inputformat=cram" ) );
				#else
				V.push_back ( std::pair<std::string,std::string> ( "inputformat=<[bam]>", "input format: bam" ) );
				#endif
				V.push_back ( std::pair<std::string,std::string> ( "ranges=<[]>", "input ranges (bam input only, default: read complete file)" ) );
				V.push_back ( std::pair<std::string,std::string> ( "exclude=<[SECONDARY,SUPPLEMENTARY]>", "exclude alignments matching any of the given flags" ) );
				V.push_back ( std::pair<std::string,std::string> ( "disablevalidation=<[0]>", "disable validation of input data" ) );
				V.push_back ( std::pair<std::string,std::string> ( "colhlog=<[18]>", "base 2 logarithm of hash table size used for collation" ) );
				V.push_back ( std::pair<std::string,std::string> ( std::string("colsbs=<[")+libmaus::util::NumberSerialisation::formatNumber(128ull*1024*1024,0)+"]>", "size of hash table overflow list in bytes" ) );
				V.push_back ( std::pair<std::string,std::string> ( std::string("T=<[") + arginfo.getDefaultTmpFileName() + "]>" , "temporary file name" ) );
				
				::biobambam::Licensing::printMap(std::cerr,V);

				std::cerr << std::endl;
				std::cerr << "Alignment flags: PAIRED,PROPER_PAIR,UNMAP,MUNMAP,REVERSE,MREVERSE,READ1,READ2,SECONDARY,QCFAIL,DUP,SUPPLEMENTARY" << std::endl;

				std::cerr << std::endl;
				return EXIT_SUCCESS;
			}
		
			
		bamtofastq(arginfo);
		
		std::cerr << "[V] " << libmaus::util::MemUsage() << " wall clock time " << rtc.formatTime(rtc.getElapsedSeconds()) << std::endl;		
	}
	catch(std::exception const & ex)
	{
		std::cerr << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
