/*
  fst - An R-package for ultra fast storage and retrieval of datasets.
  Copyright (C) 2017, Mark AJ Klik

  BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:

  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above
    copyright notice, this list of conditions and the following disclaimer
    in the documentation and/or other materials provided with the
    distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  You can contact the author at :
  - fst source repository : https://github.com/fstPackage/fst
*/


#include <iostream>
#include <fstream>
#include <stdexcept>
#include <cstring>
#include <algorithm>

#include <interface/istringwriter.h>
#include <interface/ifsttable.h>
#include <interface/icolumnfactory.h>
#include <interface/fstdefines.h>
#include <interface/fststore.h>

#include <character/character_v6.h>
#include <factor/factor_v7.h>
#include <integer/integer_v8.h>
#include <double/double_v9.h>
#include <logical/logical_v10.h>


using namespace std;



// Table metadata
//
//  NR OF BYTES            | TYPE               | VARIABLE NAME
//
//  8                      | unsigned long long | FST_FILE_ID
//  4                      | unsigned int       | FST_VERSION
//  4                      | int                | tableClassType
//  4                      | int                | keyLength
//  4                      | int                | nrOfCols  (duplicate for fast access)
//  4 * keyLength          | int                | keyColPos
//
// Column chunkset info
//
//  8                      | unsigned long long | nextHorzChunkSet
//  8                      | unsigned long long | nextVertChunkSet
//  8                      | unsigned long long | nrOfRows
//  4                      | unsigned int       | FST_VERSION
//  4                      | int                | nrOfCols
//  2 * nrOfCols           | unsigned short int | colAttributesType (not implemented yet)
//  2 * nrOfCols           | unsigned short int | colTypes
//  2 * nrOfCols           | unsigned short int | colBaseTypes
//  ?                      | char               | colNames
//
// Data chunkset index
//
//  8 * 8 (index rows)     | unsigned long long | chunkPos
//  8 * 8 (index rows)     | unsigned long long | chunkRows
//  8                      | unsigned long long | nrOfChunksPerIndexRow
//  8                      | unsigned long long | nrOfChunks
//
// Data chunk columnar position data
//
//  8 * nrOfCols           | unsigned long long | positionData
//
//

FstStore::FstStore(std::string fstFile)
{
  this->fstFile = fstFile;
  metaDataBlock = nullptr;
  blockReader = nullptr;
}


// Read header information
inline unsigned int ReadHeader(ifstream &myfile, unsigned int &tableClassType, int &keyLength, int &nrOfColsFirstChunk)
{
  // Get meta-information for table
  char tableMeta[TABLE_META_SIZE];
  myfile.read(tableMeta, TABLE_META_SIZE);

  if (!myfile)
  {
    myfile.close();
    throw(runtime_error("Error reading file header, your fst file is incomplete or damaged."));
  }


  unsigned long long* p_fstFileID = (unsigned long long*) tableMeta;
  unsigned int* p_table_version   = (unsigned int*) &tableMeta[8];
  // unsigned int* p_tableClassType  = (unsigned int*) &tableMeta[12];
  int* p_keyLength                = (int*) &tableMeta[16];
  int* p_nrOfColsFirstChunk       = (int*) &tableMeta[20];

  keyLength          = *p_keyLength;
  nrOfColsFirstChunk = *p_nrOfColsFirstChunk;

  // Without a proper file ID, we may be looking at a fst v0.7.2 file format
  if (*p_fstFileID != FST_FILE_ID)
  {
    return 0;
  }

  // Compare file version with current
  if (*p_table_version > FST_VERSION)
  {
    myfile.close();
    throw(runtime_error("Incompatible fst file: file was created by a newer version of the fst package."));
  }

  return *p_table_version;
}


inline void SetKeyIndex(vector<int> &keyIndex, int keyLength, int nrOfSelect, int* keyColPos, int* colIndex)
{
  for (int i = 0; i < keyLength; ++i)
  {
    int colSel = 0;

    for (; colSel < nrOfSelect; ++colSel)
    {
      if (keyColPos[i] == colIndex[colSel])  // key present in result
      {
        keyIndex.push_back(colSel);
        break;
      }
    }

    // key column not selected
    if (colSel == nrOfSelect) return;
  }
}


void FstStore::fstWrite(IFstTable &fstTable, int compress) const
{
  // SEXP keyNames = Rf_getAttrib(table, Rf_mkString("sorted"));

  // Meta on dataset
  int nrOfCols =  fstTable.NrOfColumns();
  int keyLength = fstTable.NrOfKeys();

  if (nrOfCols == 0)
  {
    throw(runtime_error("Your dataset needs at least one column."));
  }


  // Table meta information
  unsigned long long metaDataSize        = 56 + 4 * keyLength + 6 * nrOfCols;  // see index above
  char* metaDataBlock                    = new char[metaDataSize];

  unsigned long long* fstFileID          = (unsigned long long*) metaDataBlock;
  unsigned int* p_table_version          = (unsigned int*) &metaDataBlock[8];
  unsigned int* p_tableClassType         = (unsigned int*) &metaDataBlock[12];
  int* p_keyLength                       = (int*) &metaDataBlock[16];
  int* p_nrOfColsFirstChunk              = (int*) &metaDataBlock[20];
  int* keyColPos                         = (int*) &metaDataBlock[24];

  unsigned int offset = 24 + 4 * keyLength;

  unsigned long long* p_nextHorzChunkSet = (unsigned long long*) &metaDataBlock[offset];
  unsigned long long* p_nextVertChunkSet = (unsigned long long*) &metaDataBlock[offset + 8];
  unsigned long long* p_nrOfRows         = (unsigned long long*) &metaDataBlock[offset + 16];
  unsigned int* p_version                = (unsigned int*) &metaDataBlock[offset + 24];
  int* p_nrOfCols                        = (int*) &metaDataBlock[offset + 28];
  // unsigned short int* colAttributeTypes  = (unsigned short int*) &metaDataBlock[offset + 32];
  unsigned short int* colTypes           = (unsigned short int*) &metaDataBlock[offset + 32 + 2 * nrOfCols];
  unsigned short int* colBaseTypes       = (unsigned short int*) &metaDataBlock[offset + 32 + 4 * nrOfCols];

  // Get key column positions
  fstTable.GetKeyColumns(keyColPos);

  *fstFileID            = FST_FILE_ID;
  *p_table_version      = FST_VERSION;
  *p_tableClassType     = 1;  // default table
  *p_keyLength          = keyLength;
  *p_nrOfColsFirstChunk = nrOfCols;

  *p_nextHorzChunkSet   = 0;
  *p_nextVertChunkSet   = 0;
  *p_version            = FST_VERSION;
  *p_nrOfCols           = nrOfCols;


  // data.frame code here for stability!

  int nrOfRows = fstTable.NrOfRows();
  *p_nrOfRows = nrOfRows;


  if (nrOfRows == 0)
  {
    delete[] metaDataBlock;
    throw(runtime_error("The dataset contains no data."));
  }


  // Create file, set fast local buffer and open
  ofstream myfile;
  char ioBuf[4096];
  myfile.rdbuf()->pubsetbuf(ioBuf, 4096);  // workaround for memory leak in ofstream
  myfile.open(fstFile.c_str(), ios::binary);

  if (myfile.fail())
  {
    delete[] metaDataBlock;
    myfile.close();
    throw(runtime_error("There was an error creating the file. Please check for a correct filename."));
  }


  // Write table meta information
  myfile.write((char*)(metaDataBlock), metaDataSize);  // table meta data

  // Serialize column names
  IStringWriter* blockRunner = fstTable.GetColNameWriter();
  fdsWriteCharVec_v6(myfile, blockRunner, 0);   // column names
  delete blockRunner;

  // TODO: Write column attributes here

  // Vertical chunkset index or index of index
  char* chunkIndex = new char[CHUNK_INDEX_SIZE + 8 * nrOfCols];

  unsigned long long* chunkPos                = (unsigned long long*) chunkIndex;
  unsigned long long* chunkRows               = (unsigned long long*) &chunkIndex[64];
  unsigned long long* p_nrOfChunksPerIndexRow = (unsigned long long*) &chunkIndex[128];
  unsigned long long* p_nrOfChunks            = (unsigned long long*) &chunkIndex[136];
  unsigned long long *positionData            = (unsigned long long*) &chunkIndex[144];  // column position index


  *p_nrOfChunksPerIndexRow = 1;
  *p_nrOfChunks            = 1;  // set to 0 if all reserved slots are used
  *chunkRows               = (unsigned long long) nrOfRows;


  // Row and column meta data
  myfile.write((char*)(chunkIndex), CHUNK_INDEX_SIZE + 8 * nrOfCols);   // file positions of column data

  // column data
  for (int colNr = 0; colNr < nrOfCols; ++colNr)
  {
    positionData[colNr] = myfile.tellp();  // current location
    FstColumnType colType = fstTable.ColumnType(colNr);
    colBaseTypes[colNr] = (unsigned short int) colType;

    // Store attributes here if any
    // unsigned int attrBlockSize = SerializeObjectAttributes(ofstream &myfile, RObject rObject, serializer);

    switch (colType)
    {
      case FstColumnType::CHARACTER:
      {
        colTypes[colNr] = 6;
		IStringWriter* blockRunner = fstTable.GetStringWriter(colNr);
        fdsWriteCharVec_v6(myfile, blockRunner, compress);   // column names
		delete blockRunner;
        break;
      }

      case FstColumnType::FACTOR:
      {
        colTypes[colNr] = 7;
        int* intP = fstTable.GetIntWriter(colNr);  // level values pointer
		IStringWriter* blockRunner = fstTable.GetLevelWriter(colNr);
        fdsWriteFactorVec_v7(myfile, intP, blockRunner, nrOfRows, compress);
		delete blockRunner;
        break;
      }

      case FstColumnType::INT_32:
      {
        colTypes[colNr] = 8;
        int* intP = fstTable.GetIntWriter(colNr);
        fdsWriteIntVec_v8(myfile, intP, nrOfRows, compress);
        break;
      }

      case FstColumnType::DOUBLE_64:
      {
        colTypes[colNr] = 9;
        double* doubleP = fstTable.GetDoubleWriter(colNr);
        fdsWriteRealVec_v9(myfile, doubleP, nrOfRows, compress);
        break;
      }

      case FstColumnType::BOOL_32:
      {
        colTypes[colNr] = 10;
        int* intP = fstTable.GetLogicalWriter(colNr);
        fdsWriteLogicalVec_v10(myfile, intP, nrOfRows, compress);
        break;
      }

      default:
        delete[] metaDataBlock;
        delete[] chunkIndex;
        myfile.close();
        throw(runtime_error("Unknown type found in column."));
    }
  }

  // update chunk position data
  *chunkPos = positionData[0] - 8 * nrOfCols;

  myfile.seekp(0);
  myfile.write((char*)(metaDataBlock), metaDataSize);  // table header

  myfile.seekp(*chunkPos - CHUNK_INDEX_SIZE);
  myfile.write((char*)(chunkIndex), CHUNK_INDEX_SIZE + 8 * nrOfCols);  // vertical chunkset index and positiondata

  myfile.close();

  // cleanup
  delete[] metaDataBlock;
  delete[] chunkIndex;
}


void FstStore::fstMeta(IColumnFactory* columnFactory)
{
  // fst file stream using a stack buffer
  ifstream myfile;
  char ioBuf[4096];
  myfile.rdbuf()->pubsetbuf(ioBuf, 4096);
  myfile.open(fstFile.c_str(), ios::binary);

  if (myfile.fail())
  {
    myfile.close();
    throw(runtime_error("There was an error opening the fst file, please check for a correct path."));
  }

  // Read variables from fst file header
  version = ReadHeader(myfile, tableClassType, keyLength, nrOfColsFirstChunk);

  // We may be looking at a fst v0.7.2 file format
  if (version == 0)
  {
    // Close and reopen (slow: fst file should be resaved to avoid)
    myfile.close();
	throw(runtime_error(FSTERROR_NON_FST_FILE));
  }


  // Continue reading table metadata
  int metaSize = 32 + 4 * keyLength + 6 * nrOfColsFirstChunk;
  metaDataBlock = new char[metaSize];
  myfile.read(metaDataBlock, metaSize);

  unsigned int tmpOffset = 4 * keyLength;

  keyColPos                                 = (int*) metaDataBlock;
  // unsigned long long* p_nextHorzChunkSet = (unsigned long long*) &metaDataBlock[tmpOffset];
  // unsigned long long* p_nextVertChunkSet = (unsigned long long*) &metaDataBlock[tmpOffset + 8];
  p_nrOfRows                                = (unsigned long long*) &metaDataBlock[tmpOffset + 16];
  // unsigned int* p_version                = (unsigned int*) &metaDataBlock[tmpOffset + 24];
  int* p_nrOfCols                           = (int*) &metaDataBlock[tmpOffset + 28];
  // unsigned short int* colAttributeTypes  = (unsigned short int*) &metaDataBlock[tmpOffset + 32];
  colTypes                                  = (unsigned short int*) &metaDataBlock[tmpOffset + 32 + 2 * nrOfColsFirstChunk];
  // unsigned short int* colBaseTypes       = (unsigned short int*) &metaDataBlock[tmpOffset + 32 + 4 * nrOfColsFirstChunk];


  nrOfCols = *p_nrOfCols;


  // Read column names
  unsigned long long offset = metaSize + TABLE_META_SIZE;

  blockReader = columnFactory->CreateStringColumn(nrOfCols);
  fdsReadCharVec_v6(myfile, blockReader, offset, 0, (unsigned int) nrOfCols, (unsigned int) nrOfCols);

  // cleanup
  myfile.close();
}


void FstStore::fstRead(IFstTable &tableReader, IStringArray* columnSelection, int startRow, int endRow, IColumnFactory* columnFactory, vector<int> &keyIndex, IStringArray* selectedCols)
{
  // fst file stream using a stack buffer
  ifstream myfile;
  char ioBuf[4096];
  myfile.rdbuf()->pubsetbuf(ioBuf, 4096);
  myfile.open(fstFile.c_str(), ios::binary);

  if (myfile.fail())
  {
    myfile.close();
    throw(runtime_error("There was an error opening the fst file, please check for a correct path."));
  }

  unsigned int tableClassType;
  int keyLength, nrOfColsFirstChunk;
  version = ReadHeader(myfile, tableClassType, keyLength, nrOfColsFirstChunk);

  // We may be looking at a fst v0.7.2 file format, TODO: return error_code
  if (version == 0)
  {
    // Close and reopen (slow: fst file should be resaved to avoid this overhead)
	  myfile.close();
	  throw(runtime_error(FSTERROR_NON_FST_FILE));
  }


  // Continue reading table metadata
  int metaSize = 32 + 4 * keyLength + 6 * nrOfColsFirstChunk;
  char* metaDataBlock = new char[metaSize];
  myfile.read(metaDataBlock, metaSize);


  int* keyColPos = (int*) metaDataBlock;

  unsigned int tmpOffset = 4 * keyLength;

  // unsigned long long* p_nextHorzChunkSet = (unsigned long long*) &metaDataBlock[tmpOffset];
  // unsigned long long* p_nextVertChunkSet = (unsigned long long*) &metaDataBlock[tmpOffset + 8];
  // unsigned long long* p_nrOfRows         = (unsigned long long*) &metaDataBlock[tmpOffset + 16];
  // unsigned int* p_version                = (unsigned int*) &metaDataBlock[tmpOffset + 24];
  int* p_nrOfCols                        = (int*) &metaDataBlock[tmpOffset + 28];
  // unsigned short int* colAttributeTypes  = (unsigned short int*) &metaDataBlock[tmpOffset + 32];
  unsigned short int* colTypes           = (unsigned short int*) &metaDataBlock[tmpOffset + 32 + 2 * nrOfColsFirstChunk];
  // unsigned short int* colBaseTypes       = (unsigned short int*) &metaDataBlock[tmpOffset + 32 + 4 * nrOfColsFirstChunk];

  int nrOfCols = *p_nrOfCols;


  // TODO: read table attributes here

  // Read column names
  unsigned long long offset = metaSize + TABLE_META_SIZE;

  // Use a pure C++ charVector implementation here for performance
  IStringColumn* blockReader = columnFactory->CreateStringColumn(nrOfCols);
  fdsReadCharVec_v6(myfile, blockReader, offset, 0, (unsigned int) nrOfCols, (unsigned int) nrOfCols);

  // SEXP colNames = ((BlockReaderChar*) blockReader)->StrVector();

  // TODO: read column attributes here


  // Vertical chunkset index or index of index
  char chunkIndex[CHUNK_INDEX_SIZE];
  myfile.read(chunkIndex, CHUNK_INDEX_SIZE);

  // unsigned long long* chunkPos                = (unsigned long long*) chunkIndex;
  unsigned long long* chunkRows               = (unsigned long long*) &chunkIndex[64];
  // unsigned long long* p_nrOfChunksPerIndexRow = (unsigned long long*) &chunkIndex[128];
  unsigned long long* p_nrOfChunks            = (unsigned long long*) &chunkIndex[136];

  // Check nrOfChunks
  if (*p_nrOfChunks > 1)
  {
    myfile.close();
    delete[] metaDataBlock;
    delete blockReader;
    throw(runtime_error("Multiple chunk read not implemented yet."));
  }


  // Start reading chunk here. TODO: loop over chunks


  // Read block positions
  unsigned long long* blockPos = new unsigned long long[nrOfCols];
  myfile.read((char*) blockPos, nrOfCols * 8);  // nrOfCols file positions


  // Determine column selection
  int *colIndex;
  int nrOfSelect = 0;

  if (columnSelection == nullptr)
  {
    colIndex = new int[nrOfCols];

    for (int colNr = 0; colNr < nrOfCols; ++colNr)
    {
      colIndex[colNr] = colNr;
    }
    nrOfSelect = nrOfCols;
  }
  else  // determine column numbers of column names
  {
    nrOfSelect = columnSelection->Length();
    colIndex = new int[nrOfSelect];
    int equal;
    for (int colSel = 0; colSel < nrOfSelect; ++colSel)
    {
      equal = -1;
      const char* str1 = columnSelection->GetElement(colSel);

      for (int colNr = 0; colNr < nrOfCols; ++colNr)
      {
        const char* str2 = blockReader->GetElement(colNr);
        if (strcmp(str1, str2) == 0)
        {
          equal = colNr;
          break;
        }
      }

      if (equal == -1)
      {
        delete[] metaDataBlock;
        delete[] blockPos;
        delete[] colIndex;
        delete blockReader;
        myfile.close();
        throw(runtime_error("Selected column not found."));
      }

      colIndex[colSel] = equal;
    }
  }


  // Check range of selected rows
  int firstRow = startRow - 1;
  int nrOfRows = *chunkRows;  // TODO: check for row numbers > INT_MAX !!!

  if (firstRow >= nrOfRows || firstRow < 0)
  {
    delete[] metaDataBlock;
    delete[] blockPos;
    delete[] colIndex;
    delete blockReader;
    myfile.close();

    if (firstRow < 0)
    {
      throw(runtime_error("Parameter fromRow should have a positive value."));
    }

    throw(runtime_error("Row selection is out of range."));
  }

  int length = nrOfRows - firstRow;


  // Determine vector length
  if (endRow != -1)
  {
    if (endRow <= firstRow)
    {
      delete[] metaDataBlock;
      delete[] blockPos;
      delete[] colIndex;
      delete blockReader;
      myfile.close();
      throw(runtime_error("Incorrect row range specified."));
    }

    length = min(endRow - firstRow, nrOfRows - firstRow);
  }

  tableReader.InitTable(nrOfSelect, length);

  for (int colSel = 0; colSel < nrOfSelect; ++colSel)
  {
    int colNr = colIndex[colSel];

    if (colNr < 0 || colNr >= nrOfCols)
    {
      delete[] metaDataBlock;
      delete[] blockPos;
      delete[] colIndex;
      delete blockReader;
      myfile.close();
      throw(runtime_error("Column selection is out of range."));
    }

    unsigned long long pos = blockPos[colNr];

    switch (colTypes[colNr])
    {
    // Character vector
      case 6:
      {
        IStringColumn* stringColumn = columnFactory->CreateStringColumn(length);
        fdsReadCharVec_v6(myfile, stringColumn, pos, firstRow, length, nrOfRows);
        tableReader.SetStringColumn(stringColumn, colSel);
        delete stringColumn;
        break;
      }

      // Integer vector
      case 8:
      {
        IIntegerColumn* integerColumn = columnFactory->CreateIntegerColumn(length);
        fdsReadIntVec_v8(myfile, integerColumn->Data(), pos, firstRow, length, nrOfRows);
        tableReader.SetIntegerColumn(integerColumn, colSel);
        delete integerColumn;
        break;
      }

      // Real vector
      case 9:
      {
        IDoubleColumn* doubleColumn = columnFactory->CreateDoubleColumn(length);
        fdsReadRealVec_v9(myfile, doubleColumn->Data(), pos, firstRow, length, nrOfRows);
        tableReader.SetDoubleColumn(doubleColumn, colSel);
        delete doubleColumn;
        break;
      }

      // Logical vector
      case 10:
      {
        ILogicalColumn* logicalColumn = columnFactory->CreateLogicalColumn(length);
        fdsReadLogicalVec_v10(myfile, logicalColumn->Data(), pos, firstRow, length, nrOfRows);
        tableReader.SetLogicalColumn(logicalColumn, colSel);
        delete logicalColumn;
        break;
      }

      // Factor vector
      case 7:
      {
        IFactorColumn* factorColumn = columnFactory->CreateFactorColumn(length);
        fdsReadFactorVec_v7(myfile, factorColumn->Levels(), factorColumn->LevelData(), pos, firstRow, length, nrOfRows);
        tableReader.SetFactorColumn(factorColumn, colSel);
        delete factorColumn;
        break;
      }

      default:
        delete[] metaDataBlock;
        delete[] blockPos;
        delete[] colIndex;
        delete blockReader;
        myfile.close();
        throw(runtime_error("Unknown type found in column."));
    }
  }

  // delete blockReaderStrVec;

  myfile.close();

  // Key index
  SetKeyIndex(keyIndex, keyLength, nrOfSelect, keyColPos, colIndex);

  selectedCols->AllocateArray(nrOfSelect);

  // Only when keys are present in result set, TODO: compute using C++ only !!!
  for (int i = 0; i < nrOfSelect; ++i)
  {
    selectedCols->SetElement(i, blockReader->GetElement(colIndex[i]));
  }

  delete[] metaDataBlock;
  delete[] blockPos;
  delete[] colIndex;
  delete blockReader;
}

//
// void FstStore::ColBind(FstTable table)
// {
//   // fst file stream using a stack buffer
//   ifstream myfile;
//   char ioBuf[4096];
//   myfile.rdbuf()->pubsetbuf(ioBuf, 4096);
//   myfile.open(fstFile.c_str(), ios::binary);
//
//   const std::vector<FstColumn*> columns = table.Columns();
//
//   if (myfile.fail())
//   {
//     myfile.close();
//     throw std::runtime_error(FSTERROR_ERROR_OPENING_FILE);
//   }
//
//   // Read fst file header
//   FstMetaData metaData;
//
//   unsigned int tableClassType;
//   int keyLength, nrOfColsFirstChunk;
//   unsigned int version = metaData.ReadHeader(myfile, tableClassType, keyLength, nrOfColsFirstChunk);
//
//   // We can't append to a fst file with version lower than 1
//   if (version == 0)
//   {
//     myfile.close();
//     throw std::runtime_error(FSTERROR_NO_APPEND);
//   }
//
//
//   // Collect meta data from horizontal chunk headers and last vertical chunk set
//   uint64_t firstHorzChunkPos = 24 + 4 * keyLength;  // hard-coded offset: beware of format changes
//   istream* fstfile  = static_cast<istream*>(&myfile);
//   metaData.Collect(*fstfile, firstHorzChunkPos);
//
//   vector<uint16_t> colTypeVec = metaData.colTypeVec;
//   uint64_t nrOfRowsPrev = metaData.nrOfRows;
//
//   // We can only append an equal amount of columns
//   if (colTypeVec.size() != columns.size())
//   {
//     myfile.close();
//     throw std::runtime_error(FSTERROR_INCORRECT_COL_COUNT);
//   }
//
//    // check for equal column types
//
//   // set compression or copy from existing meta data
//
//   // Seek to last horizontal chunk index
//
//   // Seek to last vertical chunk
//
//   // Add chunk
//
//   // Update vertical chunk index of lat horizontal chunk
//
// }
//

